#ifndef CAFFE2_OPERATORS_PREFETCH_OP_H_
#define CAFFE2_OPERATORS_PREFETCH_OP_H_

#include <thread>  // NOLINT
#include <mutex>
#include <condition_variable>

#include "caffe2/core/context.h"
#include "caffe2/core/operator.h"

namespace caffe2 {

// PrefetchOperator is an operator that prefetches the next batch. It should
// almost always be used to read things from disk, so I am setting the input to
// zero blobs.
//
// For any operator that is derived from PrefetchOperator, it should
// explicitly call the Finalize() function in its destructor, so that the
// prefetching thread is properly destructed.
template <class Context>
class PrefetchOperator : public OperatorBase {
 public:
  PrefetchOperator(const OperatorDef& operator_def, Workspace* ws)
      : OperatorBase(operator_def, ws),
        context_(operator_def.device_option()),
        prefetched_(false),
        prefetch_success_(true),
        finalize_(false) {}

  virtual ~PrefetchOperator() {
    CAFFE_CHECK(finalize_)
        << "Your derived class should call Finalize() in its destructor "
           "so the prefetching thread is joined. ";
  }

  void Finalize() {
    if (prefetch_thread_.get()) {
      {
        std::unique_lock<std::mutex> lock(prefetch_access_mutex_);
        while (!prefetched_) consumer_.wait(lock);
        finalize_ = true;
        prefetched_ = false;
      }
      producer_.notify_one();
      prefetch_thread_->join();
      prefetch_thread_.reset();
    } else {
      // If we never initialized the prefetch thread, just set
      // finalize anyway.
      finalize_ = true;
    }
  }

  bool Run() override {
    // Note(jiayq): We only start the prefetch_thread at the Run() function
    // instead of in the constructor, because the prefetch_thread needs to start
    // after all derived classes' constructors finish.
    if (!prefetch_thread_) {
      prefetch_thread_.reset(
          new std::thread([this] { this->PrefetchWorker(); }));
    }
    context_.SwitchToDevice();
    std::unique_lock<std::mutex> lock(prefetch_access_mutex_);
    while (!prefetched_) consumer_.wait(lock);
    if (!prefetch_success_) {
      CAFFE_LOG_ERROR << "Prefetching failed.";
      return false;
    }
    if (!CopyPrefetched()) {
      CAFFE_LOG_ERROR << "Error when copying prefetched data.";
      return false;
    }
    prefetched_ = false;
    bool success = context_.FinishDeviceComputation();
    producer_.notify_one();
    return success;
  }

  void PrefetchWorker() {
    context_.SwitchToDevice();
    std::unique_lock<std::mutex> lock(prefetch_access_mutex_);
    while (prefetched_) producer_.wait(lock);
    while (!finalize_) {
      // Note(jiayq): we don't do FinishDeviceComputation here, because the
      // prefetcher should be using the same context. If your prefetcher uses
      // device asynchrony, make sure you manually do synchronization.
      prefetch_success_ = Prefetch();
      prefetched_ = true;
      consumer_.notify_one();
      while (prefetched_) producer_.wait(lock);
    }
  }

  // You will need to implement this instead of the Run function.
  virtual bool Prefetch() = 0;
  virtual bool CopyPrefetched() = 0;

 protected:
  Context context_;
  std::mutex prefetch_access_mutex_;
  std::condition_variable producer_, consumer_;
  // prefetched_ is used to tell the operator that it is done.
  std::atomic<bool> prefetched_;
  // prefetch_success_ is used to see if prefetching failed or not.
  std::atomic<bool> prefetch_success_;
  // finalize_ is used to tell the prefetcher to quit.
  std::atomic<bool> finalize_;
  unique_ptr<std::thread> prefetch_thread_;

  INPUT_OUTPUT_STATS(0, 0, 1, INT_MAX);
  DISABLE_COPY_AND_ASSIGN(PrefetchOperator);
};

}  // namespace caffe2

#endif  // CAFFE2_OPERATORS_PREFETCH_OP_H_
