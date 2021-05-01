/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman.h"
#include <folly/Synchronized.h>
#include <condition_variable>
#include <mutex>
#include "InMemoryView.h"
#include "fsevents.h"
#include "kqueue.h"

#if HAVE_FSEVENTS && defined(HAVE_KQUEUE)
namespace watchman {

class PendingEventsCond {
 public:
  /**
   * Notify that some events are pending.
   *
   * Return true if this thread should stop, false otherwise.
   */
  bool notifyOneOrStop() {
    auto lock = stop_.lock();
    if (lock->shouldStop) {
      return true;
    }
    lock->hasPending = true;
    cond_.notify_one();
    return false;
  }

  /**
   * Whether this thread should stop.
   */
  bool shouldStop() {
    return stop_.lock()->shouldStop;
  }

  /**
   * Wait for a change from a nested watcher. Return true if some events are
   * pending.
   */
  bool wait(int timeoutms) {
    auto lock = stop_.lock();
    if (lock->shouldStop) {
      return false;
    }
    cond_.wait_for(lock.getUniqueLock(), std::chrono::milliseconds(timeoutms));
    return lock->hasPending;
  }

  /**
   * Notify all the waiting threads to stop.
   */
  void stopAll() {
    auto lock = stop_.lock();
    lock->shouldStop = true;
    cond_.notify_all();
  }

 private:
  struct Inner {
    bool shouldStop{};
    bool hasPending{};
  };

  folly::Synchronized<Inner, std::mutex> stop_;
  std::condition_variable cond_;
};

/**
 * Watcher that uses both kqueue and fsevents to watch a hierarchy.
 *
 * The kqueue watches are used on the root directory and all the files at the
 * root, while the fsevents one is used on the subdirectories.
 */
class KQueueAndFSEventsWatcher : public Watcher {
 public:
  explicit KQueueAndFSEventsWatcher(watchman_root* root);

  bool start(const std::shared_ptr<watchman_root>& root) override;

  std::unique_ptr<watchman_dir_handle> startWatchDir(
      const std::shared_ptr<watchman_root>& root,
      struct watchman_dir* dir,
      const char* path) override;

  bool startWatchFile(struct watchman_file* file) override;

  Watcher::ConsumeNotifyRet consumeNotify(
      const std::shared_ptr<watchman_root>& root,
      PendingCollection::LockedPtr& coll) override;

  bool waitNotify(int timeoutms) override;
  void signalThreads() override;

  /**
   * Force a recrawl to be injected in the stream. Used in the
   * 'debug-kqueue-and-fsevents-recrawl' command.
   */
  void injectRecrawl(w_string path);

 private:
  folly::Synchronized<
      std::unordered_map<w_string, std::shared_ptr<FSEventsWatcher>>>
      fseventWatchers_;
  std::shared_ptr<KQueueWatcher> kqueueWatcher_;

  std::shared_ptr<PendingEventsCond> pendingCondition_;

  folly::Synchronized<std::optional<w_string>> injectedRecrawl_;
};

KQueueAndFSEventsWatcher::KQueueAndFSEventsWatcher(watchman_root* root)
    : Watcher(
          "kqueue+fsevents",
          WATCHER_ONLY_DIRECTORY_NOTIFICATIONS | WATCHER_HAS_SPLIT_WATCH),
      kqueueWatcher_(std::make_shared<KQueueWatcher>(root, false)),
      pendingCondition_(std::make_shared<PendingEventsCond>()) {}

namespace {
bool startThread(
    const std::shared_ptr<watchman_root>& root,
    const std::shared_ptr<Watcher>& watcher,
    const std::shared_ptr<PendingEventsCond>& cond) {
  std::weak_ptr<Watcher> weakWatcher(watcher);
  std::thread thr([weakWatcher, root, cond]() {
    while (true) {
      auto watcher = weakWatcher.lock();
      if (!watcher) {
        break;
      }
      if (watcher->waitNotify(86400)) {
        if (cond->notifyOneOrStop()) {
          return;
        }
      } else if (cond->shouldStop()) {
        return;
      }
    }
  });
  thr.detach();
  return true;
}
} // namespace

bool KQueueAndFSEventsWatcher::start(
    const std::shared_ptr<watchman_root>& root) {
  root->cookies.addCookieDir(root->root_path);
  return startThread(root, kqueueWatcher_, pendingCondition_);
}

std::unique_ptr<watchman_dir_handle> KQueueAndFSEventsWatcher::startWatchDir(
    const std::shared_ptr<watchman_root>& root,
    struct watchman_dir* dir,
    const char* path) {
  if (!dir->parent) {
    logf(DBG, "Watching root directory with kqueue\n");
    // This is the root, let's watch it with kqueue.
    kqueueWatcher_->startWatchDir(root, dir, path);
  } else if (dir->parent->getFullPath() == root->root_path) {
    auto fullPath = dir->getFullPath();
    auto wlock = fseventWatchers_.wlock();
    if (wlock->find(fullPath) == wlock->end()) {
      logf(
          DBG,
          "Creating a new FSEventsWatcher for top-level directory {}\n",
          dir->name);
      root->cookies.addCookieDir(fullPath);
      auto [it, _] = wlock->emplace(
          fullPath,
          std::make_shared<FSEventsWatcher>(false, std::optional(fullPath)));
      const auto& watcher = it->second;
      if (!watcher->start(root)) {
        throw std::runtime_error("couldn't start fsEvent");
      }
      if (!startThread(root, watcher, pendingCondition_)) {
        throw std::runtime_error("couldn't start fsEvent");
      }
    }
  }

  return w_dir_open(path);
}

bool KQueueAndFSEventsWatcher::startWatchFile(struct watchman_file* file) {
  if (file->parent->parent == nullptr) {
    // File at the root, watch it with kqueue.
    return kqueueWatcher_->startWatchFile(file);
  }

  // FSEvent by default watches all the files recursively, we don't need to do
  // anything.
  return true;
}

Watcher::ConsumeNotifyRet KQueueAndFSEventsWatcher::consumeNotify(
    const std::shared_ptr<watchman_root>& root,
    PendingCollection::LockedPtr& coll) {
  bool ret = false;
  {
    auto guard = injectedRecrawl_.wlock();
    if (guard->has_value()) {
      const auto& injectedDir = guard->value();

      struct timeval now;
      gettimeofday(&now, nullptr);

      coll->add(
          injectedDir,
          now,
          W_PENDING_VIA_NOTIFY | W_PENDING_RECURSIVE | W_PENDING_IS_DESYNCED);

      guard->reset();
    }
  }
  {
    auto fseventWatches = fseventWatchers_.wlock();
    for (auto& [watchpath, fsevent] : *fseventWatches) {
      auto [addedPending, cancelSelf] = fsevent->consumeNotify(root, coll);
      if (cancelSelf) {
        fsevent->signalThreads();
        root->cookies.removeCookieDir(watchpath);
        fseventWatches->erase(watchpath);
        continue;
      }
      ret |= addedPending;
    }
  }
  auto [addedPending, cancelSelf] = kqueueWatcher_->consumeNotify(root, coll);
  ret |= addedPending;
  return {ret, cancelSelf};
}

bool KQueueAndFSEventsWatcher::waitNotify(int timeoutms) {
  return pendingCondition_->wait(timeoutms);
}

void KQueueAndFSEventsWatcher::signalThreads() {
  pendingCondition_->stopAll();
  {
    auto fseventWatches = fseventWatchers_.rlock();
    for (auto& [_, fsevent] : *fseventWatches) {
      fsevent->signalThreads();
    }
  }
  kqueueWatcher_->signalThreads();
}

void KQueueAndFSEventsWatcher::injectRecrawl(w_string path) {
  *injectedRecrawl_.wlock() = path;
  pendingCondition_->notifyOneOrStop();
}

namespace {
std::shared_ptr<InMemoryView> makeKQueueAndFSEventsWatcher(
    watchman_root* root) {
  if (root->config.getBool("prefer_split_fsevents_watcher", false)) {
    return std::make_shared<InMemoryView>(
        root, std::make_shared<KQueueAndFSEventsWatcher>(root));
  } else {
    throw std::runtime_error(
        "Not using the kqueue+fsevents watcher as the \"prefer_split_fsevents_watcher\" config isn't set");
  }
}
} // namespace

static WatcherRegistry reg("kqueue+fsevents", makeKQueueAndFSEventsWatcher, 5);

namespace {

std::shared_ptr<KQueueAndFSEventsWatcher> watcherFromRoot(
    const std::shared_ptr<watchman_root>& root) {
  auto view = std::dynamic_pointer_cast<watchman::InMemoryView>(root->view());
  if (!view) {
    return nullptr;
  }

  return std::dynamic_pointer_cast<KQueueAndFSEventsWatcher>(
      view->getWatcher());
}

static void cmd_debug_kqueue_and_fsevents_recrawl(
    struct watchman_client* client,
    const json_ref& args) {
  /* resolve the root */
  if (json_array_size(args) != 3) {
    send_error_response(
        client,
        "wrong number of arguments for 'debug-kqueue-and-fsevents-recrawl'");
    return;
  }

  auto root = resolveRoot(client, args);

  auto watcher = watcherFromRoot(root);
  if (!watcher) {
    send_error_response(
        client, "root is not using the kqueue+fsevents watcher");
    return;
  }

  /* Get the path that the recrawl should be triggered on */
  const auto& json_path = args.at(2);
  auto path = json_string_value(json_path);
  if (!path) {
    send_error_response(
        client,
        "invalid value for argument 2, expected a string naming the path to trigger a recrawl on");
  }

  watcher->injectRecrawl(path);

  auto resp = make_response();
  send_and_dispose_response(client, std::move(resp));
}

} // namespace

W_CMD_REG(
    "debug-kqueue-and-fsevents-recrawl",
    cmd_debug_kqueue_and_fsevents_recrawl,
    CMD_DAEMON,
    w_cmd_realpath_root)

} // namespace watchman

#endif
