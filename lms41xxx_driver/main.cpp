#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "sick_scan_xd_api/sick_scan_api.h"

// ============ 全局状态 ============
static std::atomic<bool> g_terminate{ false };

// ============ 点云数据结构 ============
struct PointXYZI {
	float x, y, z, intensity;
};

struct PointCloudFrame {
	uint64_t timestamp_ns;
	std::vector<PointXYZI> points;
};

// ============ 线程安全队列 ============
class ThreadSafeQueue {
public:
	void push(PointCloudFrame frame) {
		{
			std::lock_guard<std::mutex> lock(mutex_);
			queue_.push(std::move(frame));
		}
		cv_.notify_one();
	}

	bool pop(PointCloudFrame &frame, std::chrono::milliseconds timeout) {
		std::unique_lock<std::mutex> lock(mutex_);
		if (!cv_.wait_for(lock, timeout, [this] { return !queue_.empty() || g_terminate.load(); })) {
			return false;
		}
		if (queue_.empty()) {
			return false;
		}

		frame = std::move(queue_.front());
		queue_.pop();
		return true;
	}

	size_t size() const {
		std::lock_guard<std::mutex> lock(mutex_);
		return queue_.size();
	}

private:
	mutable std::mutex mutex_;
	std::condition_variable cv_;
	std::queue<PointCloudFrame> queue_;
};

// 全局队列实例
static ThreadSafeQueue g_queue;

// ============ 信号处理 ============
static void SignalHandler(int sig) {
	std::cerr << "\n[Signal] Received signal " << sig << ", shutting down...\n";
	g_terminate.store(true, std::memory_order_release);
}

// ============ 点云回调 (生产者) ============
void pointCloudCallback(SickScanApiHandle apiHandle, const SickScanPointCloudMsg *msg) {
	// 解析字段偏移
	size_t offset_x = 0, offset_y = 0, offset_z = 0, offset_i = 0;
	for (int n = 0; n < msg->fields.size; n++) {
		const auto &field = msg->fields.buffer[n];
		if (field.datatype != SICK_SCAN_POINTFIELD_DATATYPE_FLOAT32)
			continue;

		if (strcmp(field.name, "x") == 0) {
			offset_x = field.offset;
		} else if (strcmp(field.name, "y") == 0) {
			offset_y = field.offset;
		} else if (strcmp(field.name, "z") == 0) {
			offset_z = field.offset;
		} else if (strcmp(field.name, "intensity") == 0) {
			offset_i = field.offset;
		}
	}

	// 构建帧数据
	PointCloudFrame frame;
	frame.timestamp_ns = msg->header.timestamp_sec * 1'000'000'000ULL + msg->header.timestamp_nsec;
	frame.points.reserve(msg->width * msg->height);

	for (size_t i = 0; i < msg->width * msg->height; i++) {
		const uint8_t *ptr = msg->data.buffer + i * msg->point_step;
		PointXYZI pt;
		pt.x = *reinterpret_cast<const float *>(ptr + offset_x);
		pt.y = *reinterpret_cast<const float *>(ptr + offset_y);
		pt.z = *reinterpret_cast<const float *>(ptr + offset_z);
		pt.intensity = *reinterpret_cast<const float *>(ptr + offset_i);
		frame.points.push_back(pt);
	}

	// 入队 (生产)
	g_queue.push(std::move(frame));
}

// ============ 文件写入线程 (消费者) ============
void writerThread(const std::string &output_path) {
	std::ofstream ofs(output_path, std::ios::binary);
	if (!ofs) {
		std::cerr << "[Writer] Failed to open: " << output_path << std::endl;
		return;
	}

	size_t frames_written = 0;
	PointCloudFrame frame;

	while (!g_terminate.load(std::memory_order_acquire)) {
		if (g_queue.pop(frame, std::chrono::milliseconds(100))) {
			// 写入时间戳
			ofs.write(reinterpret_cast<const char *>(&frame.timestamp_ns), sizeof(frame.timestamp_ns));

			// 写入点数
			uint32_t num_points = static_cast<uint32_t>(frame.points.size());
			ofs.write(reinterpret_cast<const char *>(&num_points), sizeof(num_points));

			// 写入点云数据
			ofs.write(reinterpret_cast<const char *>(frame.points.data()), frame.points.size() * sizeof(PointXYZI));

			frames_written++;
			if (frames_written % 100 == 0) {
				std::cout << "[Writer] Frames: " << frames_written << ", Queue: " << g_queue.size() << std::endl;
			}
		}
	}

	// 处理队列中剩余数据
	while (g_queue.pop(frame, std::chrono::milliseconds(0))) {
		ofs.write(reinterpret_cast<const char *>(&frame.timestamp_ns), sizeof(frame.timestamp_ns));
		uint32_t num_points = static_cast<uint32_t>(frame.points.size());
		ofs.write(reinterpret_cast<const char *>(&num_points), sizeof(num_points));
		ofs.write(reinterpret_cast<const char *>(frame.points.data()), frame.points.size() * sizeof(PointXYZI));
		frames_written++;
	}

	std::cout << "[Writer] Total frames written: " << frames_written << std::endl;
}

// ============ 主函数 ============
int main(int argc, char *argv[]) {
	// 注册信号
	std::signal(SIGINT, SignalHandler);
	std::signal(SIGTERM, SignalHandler);

	// 默认参数
	const char *launch_file = "../config/sick_lms_4xxx.launch";
	const char *output_file = "./pointcloud.bin";

	if (argc > 1)
		launch_file = argv[1];
	if (argc > 2)
		output_file = argv[2];

	// 加载库
	const char *lib_path = "../build/_deps/sick_scan_xd-build/libsick_scan_xd_shared_lib.so";
	if (SickScanApiLoadLibrary(lib_path) != SICK_SCAN_API_SUCCESS) {
		std::cerr << "Failed to load library" << std::endl;
		return 1;
	}

	// 创建并初始化
	const char *cli_args[] = { "app", launch_file };
	SickScanApiHandle apiHandle = SickScanApiCreate(2, const_cast<char **>(cli_args));
	if (SickScanApiInitByCli(apiHandle, 2, const_cast<char **>(cli_args)) != SICK_SCAN_API_SUCCESS) {
		std::cerr << "Failed to initialize" << std::endl;
		return 1;
	}

	// 启动写入线程 (消费者)
	std::thread writer(writerThread, output_file);

	// 注册回调 (生产者)
	SickScanApiRegisterCartesianPointCloudMsg(apiHandle, &pointCloudCallback);

	std::cout << "[Main] Running... Press Ctrl+C to stop" << std::endl;

	// 主循环
	while (!g_terminate.load(std::memory_order_acquire)) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	// 清理
	std::cout << "[Main] Shutting down..." << std::endl;
	SickScanApiDeregisterCartesianPointCloudMsg(apiHandle, &pointCloudCallback);

	// 等待写入线程完成
	writer.join();

	SickScanApiClose(apiHandle);
	SickScanApiRelease(apiHandle);
	SickScanApiUnloadLibrary();

	return 0;
}
