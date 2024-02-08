#pragma once

#include "pch.hpp"
#include "lockfree/queue.hpp"

#ifdef _WIN32
#include <Windows.h>
#endif

namespace tasky
{
	struct DefaultAllocator
	{
		static void* alloc(std::size_t size) { return ::malloc(size); }
		static void free(void* ptr) { ::free(ptr); }
	};

	template<typename T, typename Allocator>
	class Task;

	template<typename Allocator>
	class Task<void, Allocator>;

	class Scheduler;

	struct PromiseBase
	{
		template<typename T>
		constexpr static std::coroutine_handle<PromiseBase> cast(std::coroutine_handle<T> handle) noexcept { return std::coroutine_handle<PromiseBase>::from_address(handle.address()); }

		constexpr std::suspend_always initial_suspend() const noexcept { return {}; }
		constexpr std::suspend_always final_suspend() const noexcept { return {}; }

		void unhandled_exception()
		{
			exception = std::current_exception();
		}

		PromiseBase() {}

		virtual ~PromiseBase() {}

		std::atomic<std::size_t> awaiting_count = 0;
		std::coroutine_handle<PromiseBase> awaiting_coro = nullptr;
		std::exception_ptr exception = nullptr;
		Scheduler* scheduler = nullptr;
	};

	class Scheduler
	{
	public:
		Scheduler(std::size_t workers = std::thread::hardware_concurrency() - 1) :
			max_workers(workers),
			queue_(1024)
		{
			std::cout << "Running scheduler with " << workers + 1 << " threads..." << std::endl;
		}

		template<typename T, typename Allocator>
		void schedule(const std::vector<Task<T, Allocator>>& tasks)
		{
			running_tasks.fetch_add(tasks.size(), std::memory_order::acq_rel);
			for (auto& t : tasks)
			{
				std::coroutine_handle<PromiseBase> h = PromiseBase::cast(t.handle);
				h.promise().scheduler = this;
				queue_.push(h);
			}
		}

		template<typename T>
		void schedule(std::coroutine_handle<T> handle)
		{
			running_tasks.fetch_add(1, std::memory_order::acq_rel);
			std::coroutine_handle<PromiseBase> h = PromiseBase::cast(handle);
			h.promise().scheduler = this;
			queue_.push(h);
		}

		template<typename T, typename Allocator>
		void schedule(const Task<T, Allocator>& task)
		{
			schedule(task.handle);
		}

		template<typename T, typename Allocator, typename... Ts>
		void schedule(const Task<T, Allocator>& task, Ts... tasks)
		{
			schedule(task.handle);
			schedule(std::forward<Ts>(tasks)...);
		}

		void run()
		{
			for (std::size_t i = 0; i < max_workers; i++)
				workers_.emplace_back([&]() { run_worker(); });

			while (running_tasks.load(std::memory_order::acquire) > 0)
				run_next_task();

			for (auto& t : workers_)
				t.join();

			workers_.clear();
		}

		void schedule_awaiting(std::coroutine_handle<PromiseBase> handle)
		{
			queue_.push(handle);
		}

	private:
		void release_task() { running_tasks.fetch_sub(1, std::memory_order::acq_rel); }

		void run_worker()
		{
			while (running_tasks.load(std::memory_order::acquire) > 1)
				run_next_task();
		}

		void run_next_task()
		{
			auto handle = next_task();

			if (handle == nullptr)
				return;

			if (!handle.done())
			{
				handle.resume();
				if (handle.done())
				{
					release_task();

					auto awaiting = handle.promise().awaiting_coro;

					if (awaiting != nullptr)
					{
						auto i = awaiting.promise().awaiting_count.fetch_sub(1, std::memory_order::acq_rel) - 1;

						if (i == 0)
							schedule_awaiting(awaiting);
					}
					else if (handle.promise().exception)
					{
						std::rethrow_exception(handle.promise().exception);
					}
					else
					{
						handle.destroy();
					}
				}
			}
			else
			{
				release_task();
			}
		}

		[[nodiscard]] std::coroutine_handle<PromiseBase> next_task()
		{
			if (queue_.size() == 0)
				return nullptr;

			std::coroutine_handle<PromiseBase> handle = nullptr;
			queue_.try_pop(handle);
			return handle;
		}

		std::atomic<std::size_t> running_tasks = 0;
		const std::size_t max_workers;
		lockfree::Queue<std::coroutine_handle<PromiseBase>> queue_;
		std::vector<std::thread> workers_ = {};
	};

	template<typename T, typename Allocator = DefaultAllocator>
	class Task
	{
	public:
		struct promise_type : public PromiseBase
		{
			[[nodiscard]] static void* operator new(std::size_t size)
			{
				return Allocator::alloc(size);
			}

			static void operator delete(void* ptr)
			{
				Allocator::free(ptr);
			}

			virtual ~promise_type() {}

			[[nodiscard]] constexpr Task<T> get_return_object() noexcept { return Task<T>(std::coroutine_handle<promise_type>::from_promise(*this)); }

			constexpr void return_value(const T& val) noexcept
			{
				if (exception)
					std::rethrow_exception(exception);
				value.emplace(std::move(val));
			}

			constexpr void return_value(T&& val)
			{
				if (exception)
					std::rethrow_exception(exception);
				value.emplace(val);
			}

			std::optional<T> value;
		};

		using Handle = std::coroutine_handle<promise_type>;

		Task(Handle handle) : handle(handle) {}
		Task(const Task& task) = delete;
		Task(Task&& task) = delete;
		~Task() {}

		[[nodiscard]] auto operator co_await() const noexcept
		{
			struct Awaiter
			{
				constexpr bool await_ready() const noexcept { return false; }

				[[nodiscard]] constexpr T await_resume() const
				{
					if (coro.promise().exception)
						std::rethrow_exception(coro.promise().exception);

					return std::move(coro.promise().value.value());
				}

				void await_suspend(std::coroutine_handle<> awaiting_handle)
				{
					auto handle = PromiseBase::cast(awaiting_handle);
					auto& promise = handle.promise();

					coro.promise().awaiting_coro = handle;
					promise.awaiting_count.store(1, std::memory_order::release);
					promise.scheduler->schedule(coro);
				}

				std::coroutine_handle<promise_type> coro;

				Awaiter(std::coroutine_handle<promise_type> coro) noexcept : coro(coro) {}
				~Awaiter() { coro.destroy(); }
			};

			return Awaiter(handle);
		}

		Handle handle;
	};

	template<typename Allocator>
	class Task<void, Allocator>
	{
	public:
		struct promise_type : public PromiseBase
		{
			[[nodiscard]] static void* operator new(std::size_t size)
			{
				return Allocator::alloc(size);
			}

			static void operator delete(void* ptr)
			{
				Allocator::free(ptr);
			}

			virtual ~promise_type() {}

			[[nodiscard]] Task<void> get_return_object() noexcept { return Task<void>(std::coroutine_handle<promise_type>::from_promise(*this)); }

			constexpr void return_void() const
			{
				if (exception)
					std::rethrow_exception(exception);
			}
		};

		using Handle = std::coroutine_handle<promise_type>;

		Task(Handle handle) : handle(handle) {}
		Task(const Task& task) = delete;
		Task(Task&& task) = delete;
		~Task() {}

		[[nodiscard]] auto operator co_await() noexcept
		{
			struct Awaiter
			{
				constexpr bool await_ready() const noexcept { return false; }

				constexpr void await_resume() const
				{
					if (coro.promise().exception)
						std::rethrow_exception(coro.promise().exception);
				}

				void await_suspend(std::coroutine_handle<> awaiting_handle)
				{
					auto handle = PromiseBase::cast(awaiting_handle);
					auto& promise = handle.promise();

					coro.promise().awaiting_coro = handle;
					promise.awaiting_count.store(1, std::memory_order::release);
					promise.scheduler->schedule(coro);
				}

				std::coroutine_handle<promise_type> coro;

				Awaiter(std::coroutine_handle<promise_type> coro) noexcept : coro(coro) {}
				~Awaiter() { coro.destroy(); }
			};

			return Awaiter(handle);
		}

		Handle handle;
	};

	template<typename T, typename Allocator = DefaultAllocator>
	struct MultipleAwaiter
	{
		constexpr bool await_ready() const noexcept { return false; }

		[[nodiscard]] constexpr std::vector<T> await_resume() const
		{
			std::vector<T> results;
			for (auto& coro : coros)
			{
				using P = Task<T, Allocator>::promise_type;
				auto x = std::coroutine_handle<P>::from_address(coro.address());
				auto& p = x.promise();

				if (p.exception)
					std::rethrow_exception(p.exception);

				results.emplace_back(p.value.value());
			}
			return results;
		}

		void await_suspend(std::coroutine_handle<> awaiting_handle)
		{
			auto handle = PromiseBase::cast(awaiting_handle);

			auto& promise = handle.promise();
			Scheduler* scheduler = promise.scheduler;
			promise.awaiting_count.store(coros.size(), std::memory_order::release);

			for (auto& coro : coros)
			{
				coro.promise().awaiting_coro = handle;
				scheduler->schedule(coro);
			}
		}

		MultipleAwaiter(std::initializer_list<Task<T, Allocator>> tasks) : coros()
		{
			for (auto& coro : tasks)
				coros.emplace_back(PromiseBase::cast(coro.handle));
		}

		MultipleAwaiter(std::vector<Task<T, Allocator>>&& tasks) : coros()
		{
			for (auto& coro : tasks)
				coros.emplace_back(PromiseBase::cast(coro.handle));
		}

		~MultipleAwaiter()
		{
			for (auto& coro : coros)
				coro.destroy();
		}

		std::vector<std::coroutine_handle<PromiseBase>> coros;
	};

	template<typename Allocator>
	struct MultipleAwaiter<void, Allocator>
	{
		[[nodiscard]] constexpr bool await_ready() const noexcept { return false; }

		constexpr void await_resume() const
		{
			for (auto& coro : coros)
			{
				auto& promise = PromiseBase::cast(coro).promise();

				if (promise.exception)
					std::rethrow_exception(promise.exception);
			}
		}

		void await_suspend(std::coroutine_handle<> awaiting_handle)
		{
			auto handle = PromiseBase::cast(awaiting_handle);

			auto& promise = handle.promise();
			Scheduler* scheduler = promise.scheduler;
			promise.awaiting_count.store(coros.size(), std::memory_order::release);

			for (auto& coro : coros)
			{
				coro.promise().awaiting_coro = handle;
				scheduler->schedule(coro);
			}
		}

		MultipleAwaiter(std::initializer_list<Task<void, Allocator>> tasks) : coros()
		{
			for (auto& coro : tasks)
				coros.emplace_back(PromiseBase::cast(coro.handle));
		}

		MultipleAwaiter(std::vector<Task<void, Allocator>>&& tasks) : coros()
		{
			for (auto& coro : tasks)
				coros.emplace_back(PromiseBase::cast(coro.handle));
		}

		~MultipleAwaiter()
		{
			for (auto& coro : coros)
				coro.destroy();
		}

		std::vector<std::coroutine_handle<PromiseBase>> coros;
	};

	template<typename T, typename Allocator = DefaultAllocator>
	[[nodiscard]] auto all(std::initializer_list<Task<T, Allocator>> elements)
	{
		return MultipleAwaiter<T, Allocator>(std::move(elements));
	};

	template<typename Allocator = DefaultAllocator>
	[[nodiscard]] auto all(std::initializer_list<Task<void, Allocator>> elements)
	{
		return MultipleAwaiter<void, Allocator>(std::move(elements));
	};

	template<typename T, typename Allocator = DefaultAllocator>
	[[nodiscard]] auto all(std::vector<Task<T, Allocator>>&& elements)
	{
		return MultipleAwaiter<T, Allocator>(std::move(elements));
	};

	template<typename Allocator = DefaultAllocator>
	[[nodiscard]] auto all(std::vector<Task<void, Allocator>>&& elements)
	{
		return MultipleAwaiter<void, Allocator>(std::move(elements));
	};

#ifdef _WIN32
	void WINAPI onFileRead(
		_In_    DWORD dwErrorCode,
		_In_    DWORD dwNumberOfBytesTransfered,
		_Inout_ LPOVERLAPPED lpOverlapped
	);

	struct ReadFileAwaiter : public OVERLAPPED
	{
		ReadFileAwaiter(const std::string& path) : OVERLAPPED(),
			data_()
		{
			fileHandle_ = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);

			if (fileHandle_ == INVALID_HANDLE_VALUE)
				throw std::runtime_error("Could not create file handle!");
		}

		bool await_ready() const noexcept
		{
			return false;
		}

		std::string await_resume() const
		{
			return std::move(data_);
		}

		void await_suspend(std::coroutine_handle<> handle)
		{
			handle_ = tasky::PromiseBase::cast(handle);

			[[maybe_unused]] DWORD fileSizeHigh = 0;
			DWORD fileSizeLow = GetFileSize(fileHandle_, &fileSizeHigh);

			data_.resize(fileSizeLow);

			if (!BindIoCompletionCallback(fileHandle_, onFileRead, 0))
				throw std::runtime_error("Could not bind IO completion callback!");

			if (!ReadFile(fileHandle_, data_.data(), fileSizeLow, &read_, this))
			{
				const int err = GetLastError();
				if (err != 997)
					throw std::runtime_error("Could not read file!");

			}
		}

	private:
		DWORD read_ = 0;
		std::string data_;
		void* fileHandle_ = nullptr;
		std::coroutine_handle<tasky::PromiseBase> handle_ = nullptr;

		friend void WINAPI onFileRead(
			_In_    DWORD dwErrorCode,
			_In_    DWORD dwNumberOfBytesTransfered,
			_Inout_ LPOVERLAPPED lpOverlapped
		);
	};

	void WINAPI onFileRead(
		_In_    DWORD dwErrorCode,
		_In_    DWORD dwNumberOfBytesTransfered,
		_Inout_ LPOVERLAPPED lpOverlapped
	)
	{
		auto* s = static_cast<ReadFileAwaiter*>(lpOverlapped);

		if (!CloseHandle(s->fileHandle_))
			throw std::runtime_error("Could not close file handle!");

		s->handle_.promise().scheduler->schedule_awaiting(s->handle_);
	}

	void WINAPI onFileWrite(
		_In_    DWORD dwErrorCode,
		_In_    DWORD dwNumberOfBytesTransfered,
		_Inout_ LPOVERLAPPED lpOverlapped
	);

	struct WriteFileAwaiter : public OVERLAPPED
	{
		WriteFileAwaiter(const std::string& path, const std::string& data) : OVERLAPPED(),
			data_(data)
		{
			fileHandle_ = CreateFileA(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);

			if (fileHandle_ == INVALID_HANDLE_VALUE)
				throw std::runtime_error("Could not create file handle!");
		}

		bool await_ready() const noexcept
		{
			return false;
		}

		void await_resume() const {}

		void await_suspend(std::coroutine_handle<> handle)
		{
			handle_ = tasky::PromiseBase::cast(handle);

			if (!BindIoCompletionCallback(fileHandle_, onFileWrite, 0))
				throw std::runtime_error("Could not bind IO completion callback!");

			DWORD s = static_cast<DWORD>(data_.size());

			if (!WriteFile(fileHandle_, data_.data(), s, &written_, this))
			{
				const int err = GetLastError();
				if (err != 997)
					throw std::runtime_error("Could not write file!");
			}
		}

	private:
		DWORD written_ = 0;
		const std::string& data_;
		void* fileHandle_ = nullptr;
		std::coroutine_handle<tasky::PromiseBase> handle_ = nullptr;

		friend void WINAPI onFileWrite(
			_In_    DWORD dwErrorCode,
			_In_    DWORD dwNumberOfBytesTransfered,
			_Inout_ LPOVERLAPPED lpOverlapped
		);
	};

	void WINAPI onFileWrite(
		_In_    DWORD dwErrorCode,
		_In_    DWORD dwNumberOfBytesTransfered,
		_Inout_ LPOVERLAPPED lpOverlapped
	)
	{
		auto* s = static_cast<WriteFileAwaiter*>(lpOverlapped);

		if (!CloseHandle(s->fileHandle_))
			throw std::runtime_error("Could not close file handle!");

		s->handle_.promise().scheduler->schedule_awaiting(s->handle_);
	}

#endif

	Task<std::string> readFile(const std::string& path)
	{
		co_return co_await ReadFileAwaiter(path);
	}

	Task<void> writeFile(const std::string& path, const std::string& data)
	{
		co_await WriteFileAwaiter(path, data);
	}
}
