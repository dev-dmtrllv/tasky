#pragma once

#include "pch.hpp"
#include "lockfree/queue.hpp"

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
		static std::coroutine_handle<PromiseBase> cast(std::coroutine_handle<T> handle) noexcept { return std::coroutine_handle<PromiseBase>::from_address(handle.address()); }

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
		void schedule(std::vector<Task<T, Allocator>> tasks)
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
		void schedule(Task<T, Allocator> task)
		{
			schedule(task.handle);
		}

		template<typename T, typename Allocator, typename... Ts>
		void schedule(Task<T, Allocator> task, Ts... tasks)
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

		std::coroutine_handle<PromiseBase> next_task()
		{
			if (queue_.size() == 0)
				return nullptr;

			std::coroutine_handle<PromiseBase> handle = nullptr;
			queue_.try_pop(handle);
			return handle;
		}

		void schedule_awaiting(std::coroutine_handle<PromiseBase> handle)
		{
			queue_.push(handle);
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
			static void* operator new(std::size_t size)
			{
				return Allocator::alloc(size);
			}

			static void operator delete(void* ptr)
			{
				Allocator::free(ptr);
			}

			virtual ~promise_type() {}

			constexpr Task<T> get_return_object() noexcept { return Task<T>(std::coroutine_handle<promise_type>::from_promise(*this)); }

			constexpr void return_value(const T& val) noexcept { value.emplace(std::move(val)); }
			constexpr void return_value(T&& val) noexcept { value.emplace(val); }

			std::optional<T> value;
		};

		using Handle = std::coroutine_handle<promise_type>;

		Task(Handle handle) : handle(handle) {}
		Task(const Task& task) = delete;
		Task(Task&& task) = delete;
		~Task() {}

		auto operator co_await()
		{
			struct Awaiter
			{
				constexpr bool await_ready() const noexcept { return false; }

				constexpr T await_resume() const noexcept
				{
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

				Awaiter(std::coroutine_handle<promise_type> coro) : coro(coro) {}
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
			static void* operator new(std::size_t size)
			{
				return Allocator::alloc(size);
			}

			static void operator delete(void* ptr)
			{
				Allocator::free(ptr);
			}

			virtual ~promise_type() {}

			Task<void> get_return_object() noexcept { return Task<void>(std::coroutine_handle<promise_type>::from_promise(*this)); }

			constexpr void return_void() const noexcept {}
		};

		using Handle = std::coroutine_handle<promise_type>;

		Task(Handle handle) : handle(handle) {}
		Task(const Task& task) = delete;
		Task(Task&& task) = delete;
		~Task() {}

		auto operator co_await()
		{
			struct Awaiter
			{
				constexpr bool await_ready() const noexcept { return false; }
				constexpr void await_resume() const noexcept {}

				void await_suspend(std::coroutine_handle<> awaiting_handle)
				{
					auto handle = PromiseBase::cast(awaiting_handle);
					auto& promise = handle.promise();

					coro.promise().awaiting_coro = handle;
					promise.awaiting_count.store(1, std::memory_order::release);
					promise.scheduler->schedule(coro);
				}

				std::coroutine_handle<promise_type> coro;

				Awaiter(std::coroutine_handle<promise_type> coro) : coro(coro) {}
				~Awaiter() { coro.destroy(); }
			};

			return Awaiter(handle);
		}

		Handle handle;
	};

	template<typename T, typename Allocator = DefaultAllocator>
	auto all(std::initializer_list<Task<T, Allocator>> elements)
	{
		struct Awaiter
		{
			constexpr bool await_ready() const noexcept { return false; }

			constexpr std::vector<T> await_resume() const noexcept
			{
				std::vector<T> results;
				for (auto& coro : coros)
				{
					using P = Task<T, Allocator>::promise_type;
					auto x = std::coroutine_handle<P>::from_address(coro.address());
					auto& p = x.promise();
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

			Awaiter(std::initializer_list<Task<T, Allocator>> tasks) : coros()
			{
				for (auto& coro : tasks)
					coros.emplace_back(PromiseBase::cast(coro.handle));
			}

			~Awaiter()
			{
				for (auto& coro : coros)
					coro.destroy();
			}

			std::vector<std::coroutine_handle<PromiseBase>> coros;
		};

		return Awaiter(std::move(elements));
	};

	template<typename Allocator = DefaultAllocator>
	auto all(std::initializer_list<Task<void, Allocator>> elements)
	{
		struct Awaiter
		{
			constexpr bool await_ready() const noexcept { return false; }

			constexpr void await_resume() const noexcept {}

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

			Awaiter(std::initializer_list<Task<void, Allocator>> tasks) : coros()
			{
				for (auto& coro : tasks)
					coros.emplace_back(PromiseBase::cast(coro.handle));
			}

			~Awaiter()
			{
				for (auto& coro : coros)
					coro.destroy();
			}

			std::vector<std::coroutine_handle<PromiseBase>> coros;
		};

		return Awaiter(std::move(elements));
	};
}
