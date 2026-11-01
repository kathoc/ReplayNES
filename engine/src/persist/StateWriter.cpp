#include "persist/StateWriter.h"

#include "util/Fs.h"
#include "util/Hash.h"

namespace rn {

StateWriter::~StateWriter() {
  {
    std::lock_guard<std::mutex> l(m_);
    quit_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void StateWriter::submit(Job j) {
  {
    std::lock_guard<std::mutex> l(m_);
    queue_.push_back(std::move(j));
    if (!thread_.joinable()) thread_ = std::thread([this] { run(); });
  }
  cv_.notify_one();
}

std::vector<StateWriter::Job> StateWriter::takeDone() {
  std::lock_guard<std::mutex> l(m_);
  std::vector<Job> out;
  out.swap(done_);
  return out;
}

void StateWriter::drain() {
  std::unique_lock<std::mutex> l(m_);
  idle_.wait(l, [this] { return queue_.empty() && !busy_; });
}

size_t StateWriter::outstanding() const {
  std::lock_guard<std::mutex> l(m_);
  return queue_.size() + done_.size() + (busy_ ? 1 : 0);
}

void StateWriter::run() {
  std::unique_lock<std::mutex> l(m_);
  for (;;) {
    cv_.wait(l, [this] { return quit_ || !queue_.empty(); });
    if (queue_.empty()) return;  // quit with nothing left
    Job j = std::move(queue_.front());
    queue_.pop_front();
    busy_ = true;
    l.unlock();
    j.crc = crc32(j.data, j.n);
    const uint32_t fileCrc = crc32(j.data, j.n, crc32(j.header.data(), j.header.size()));
    const uint8_t trailer[4] = {uint8_t(fileCrc), uint8_t(fileCrc >> 8), uint8_t(fileCrc >> 16), uint8_t(fileCrc >> 24)};
    j.status = fs::writeFileAtomic(j.path, {{j.header.data(), j.header.size()}, {j.data, j.n}, {trailer, 4}});
    l.lock();
    done_.push_back(std::move(j));
    busy_ = false;
    if (queue_.empty()) idle_.notify_all();
  }
}

}  // namespace rn
