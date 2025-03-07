/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/root/watchlist.h"
#include <folly/Synchronized.h>
#include <vector>
#include "watchman/TriggerCommand.h"
#include "watchman/query/Query.h"
#include "watchman/query/QueryContext.h"
#include "watchman/root/Root.h"

namespace watchman {

folly::Synchronized<std::unordered_map<w_string, std::shared_ptr<Root>>>
    watched_roots;
std::atomic<long> live_roots{0};

bool Root::removeFromWatched() {
  auto map = watched_roots.wlock();
  auto it = map->find(root_path);
  if (it == map->end()) {
    return false;
  }
  // it's possible that the root has already been removed and replaced with
  // another, so make sure we're removing the right object
  if (it->second.get() == this) {
    map->erase(it);
    return true;
  }
  return false;
}

// Given a filename, walk the current set of watches.
// If a watch is a prefix match for filename then we consider it to
// be an enclosing watch and we'll return the root path and the relative
// path to filename.
// Returns NULL if there were no matches.
// If multiple watches have the same prefix, it is undefined which one will
// match.
bool findEnclosingRoot(
    const w_string& fileName,
    w_string_piece& prefix,
    w_string_piece& relativePath) {
  std::shared_ptr<Root> root;
  auto name = fileName.piece();
  {
    auto map = watched_roots.rlock();
    for (const auto& it : *map) {
      auto root_name = it.first;
      if (name.startsWith(root_name.piece()) &&
          (name.size() == root_name.size() /* exact match */ ||
           is_slash(name[root_name.size()] /* dir container matches */))) {
        root = it.second;
        prefix = root_name.piece();
        if (name.size() == root_name.size()) {
          relativePath = w_string_piece();
        } else {
          relativePath = name;
          relativePath.advance(root_name.size() + 1);
        }
        return true;
      }
    }
  }
  return false;
}

json_ref w_root_stop_watch_all() {
  std::vector<Root*> roots;
  json_ref stopped = json_array();

  Root::SaveGlobalStateHook saveGlobalStateHook = nullptr;

  // Funky looking loop because root->cancel() needs to acquire the
  // watched_roots wlock and will invalidate any iterators we might
  // otherwise have held.  Therefore we just loop until the map is
  // empty.
  while (true) {
    std::shared_ptr<Root> root;

    {
      auto map = watched_roots.wlock();
      if (map->empty()) {
        break;
      }

      auto it = map->begin();
      root = it->second;
    }

    root->cancel();
    if (!saveGlobalStateHook) {
      saveGlobalStateHook = root->getSaveGlobalStateHook();
    } else {
      // This assumes there is only one hook per application, rather than
      // independent hooks per root. That's true today because every root holds
      // w_state_save.
      w_assert(
          saveGlobalStateHook == root->getSaveGlobalStateHook(),
          "all roots must contain the same saveGlobalStateHook");
    }
    json_array_append_new(stopped, w_string_to_json(root->root_path));
  }

  if (saveGlobalStateHook) {
    saveGlobalStateHook();
  }

  return stopped;
}

json_ref w_root_watch_list_to_json() {
  auto arr = json_array();

  auto map = watched_roots.rlock();
  for (const auto& it : *map) {
    auto root = it.second;
    json_array_append_new(arr, w_string_to_json(root->root_path));
  }

  return arr;
}

json_ref Root::getStatusForAllRoots() {
  auto arr = json_array();

  auto map = watched_roots.rlock();
  for (const auto& it : *map) {
    auto root = it.second;
    json_array_append_new(arr, root->getStatus());
  }

  return arr;
}

json_ref Root::getStatus() const {
  auto obj = json_object();
  auto now = std::chrono::steady_clock::now();

  auto cookie_array = json_array();
  for (auto& name : cookies.getOutstandingCookieFileList()) {
    cookie_array.array().push_back(w_string_to_json(name));
  }

  std::string crawl_status;
  auto recrawl_info = json_object();
  {
    auto info = recrawlInfo.rlock();
    recrawl_info.set({
        {"count", json_integer(info->recrawlCount)},
        {"should-recrawl", json_boolean(info->shouldRecrawl)},
        {"warning", w_string_to_json(info->warning)},
    });

    if (!inner.done_initial) {
      crawl_status = folly::to<std::string>(
          info->recrawlCount ? "re-" : "",
          "crawling for ",
          std::chrono::duration_cast<std::chrono::milliseconds>(
              now - info->crawlStart)
              .count(),
          "ms");
    } else if (info->shouldRecrawl) {
      crawl_status = folly::to<std::string>(
          "needs recrawl: ",
          info->warning.view(),
          ". Last crawl was ",
          std::chrono::duration_cast<std::chrono::milliseconds>(
              now - info->crawlFinish)
              .count(),
          "ms ago");
    } else {
      crawl_status = folly::to<std::string>(
          "crawl completed ",
          std::chrono::duration_cast<std::chrono::milliseconds>(
              now - info->crawlFinish)
              .count(),
          "ms ago, and took ",
          std::chrono::duration_cast<std::chrono::milliseconds>(
              info->crawlFinish - info->crawlStart)
              .count(),
          "ms");
    }
  }

  auto query_info = json_array();
  {
    auto locked = queries.rlock();
    for (auto& ctx : *locked) {
      auto info = json_object();
      auto elapsed = now - ctx->created;

      const char* queryState = "?";
      switch (ctx->state.load()) {
        case QueryContextState::NotStarted:
          queryState = "NotStarted";
          break;
        case QueryContextState::WaitingForCookieSync:
          queryState = "WaitingForCookieSync";
          break;
        case QueryContextState::WaitingForViewLock:
          queryState = "WaitingForViewLock";
          break;
        case QueryContextState::Generating:
          queryState = "Generating";
          break;
        case QueryContextState::Rendering:
          queryState = "Rendering";
          break;
        case QueryContextState::Completed:
          queryState = "Completed";
          break;
      }

      info.set({
          {"elapsed-milliseconds",
           json_integer(
               std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                   .count())},
          {"cookie-sync-duration-milliseconds",
           json_integer(ctx->cookieSyncDuration.load().count())},
          {"generation-duration-milliseconds",
           json_integer(ctx->generationDuration.load().count())},
          {"render-duration-milliseconds",
           json_integer(ctx->renderDuration.load().count())},
          {"view-lock-wait-duration-milliseconds",
           json_integer(ctx->viewLockWaitDuration.load().count())},
          {"state", typed_string_to_json(queryState)},
          {"client-pid", json_integer(ctx->query->clientPid)},
          {"request-id", w_string_to_json(ctx->query->request_id)},
          {"query", json_ref(ctx->query->query_spec)},
      });
      if (ctx->query->subscriptionName) {
        info.set(
            "subscription-name",
            w_string_to_json(ctx->query->subscriptionName));
      }

      query_info.array().push_back(info);
    }
  }

  auto cookiePrefix = cookies.cookiePrefix();
  auto jsonCookiePrefix = json_array();
  for (const auto& name : cookiePrefix) {
    jsonCookiePrefix.array().push_back(w_string_to_json(name));
  }

  auto cookieDirs = cookies.cookieDirs();
  auto jsonCookieDirs = json_array();
  for (const auto& dir : cookieDirs) {
    jsonCookieDirs.array().push_back(w_string_to_json(dir));
  }

  obj.set({
      {"path", w_string_to_json(root_path)},
      {"fstype", w_string_to_json(fs_type)},
      {"case_sensitive",
       json_boolean(case_sensitive == CaseSensitivity::CaseSensitive)},
      {"cookie_prefix", std::move(jsonCookiePrefix)},
      {"cookie_dir", std::move(jsonCookieDirs)},
      {"cookie_list", std::move(cookie_array)},
      {"recrawl_info", std::move(recrawl_info)},
      {"queries", std::move(query_info)},
      {"done_initial", json_boolean(inner.done_initial)},
      {"cancelled", json_boolean(inner.cancelled)},
      {"crawl-status",
       w_string_to_json(w_string(crawl_status.data(), crawl_status.size()))},
  });
  return obj;
}

json_ref Root::triggerListToJson() const {
  auto arr = json_array();
  {
    auto map = triggers.rlock();
    for (const auto& it : *map) {
      const auto& cmd = it.second;
      json_array_append(arr, cmd->definition);
    }
  }

  return arr;
}

void w_root_free_watched_roots() {
  int last, interval;
  time_t started;
  std::vector<std::shared_ptr<Root>> roots;

  // We want to cancel the list of roots, but need to be careful to avoid
  // deadlock; make a copy of the set of roots under the lock...
  {
    auto map = watched_roots.rlock();
    for (const auto& it : *map) {
      roots.emplace_back(it.second);
    }
  }

  // ... and cancel them outside of the lock
  for (auto& root : roots) {
    if (!root->cancel()) {
      root->stopThreads();
    }
  }

  // release them all so that we don't mess with the number of live_roots
  // in the code below.
  roots.clear();

  last = live_roots;
  time(&started);
  logf(DBG, "waiting for roots to cancel and go away {}\n", last);
  interval = 100;
  for (;;) {
    auto current = live_roots.load();
    if (current == 0) {
      break;
    }
    if (time(NULL) > started + 3) {
      logf(ERR, "{} roots were still live at exit\n", current);
      break;
    }
    if (current != last) {
      logf(DBG, "waiting: {} live\n", current);
      last = current;
    }
    /* sleep override */ std::this_thread::sleep_for(
        std::chrono::microseconds(interval));
    interval = std::min(interval * 2, 1000000);
  }

  logf(DBG, "all roots are gone\n");
}

} // namespace watchman

/* vim:ts=2:sw=2:et:
 */
