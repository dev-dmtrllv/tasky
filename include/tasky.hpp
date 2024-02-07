#pragma once

#include "pch.hpp"
#include "lockfree/queue.hpp"

namespace tasky
{
	template<typename T>
	class Task;

	template<>
	class Task<void>;

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

		virtual ~PromiseBase() = 0 {}

		std::coroutine_handle<PromiseBase> awaiting_coro;
		std::exception_ptr exception = nullptr;
		Scheduler* scheduler;
	};




	class Scheduler
	{
	public:
		Scheduler(std::size_t workers = std::thread::hardware_concurrency() - 1) :
			max_workers(workers),
			queue_(1024)
		{

		}

		template<typename T>
		void schedule(std::coroutine_handle<T> handle)
		{
			running_tasks.fetch_add(1, std::memory_order::acq_rel);
			std::coroutine_handle<PromiseBase> h = PromiseBase::cast(handle);
			h.promise().scheduler = this;
			queue_.push(h);
		}

		template<typename T>
		void schedule(Task<T> task)
		{
			schedule(task.handle);
		}

		template<typename T, typename... Ts>
		void schedule(Task<T> task, Ts... tasks)
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

			std::cout << "thread done..." << std::endl;
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
						schedule_awaiting(awaiting);
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

	template<typename T>
	class Task
	{
	public:
		struct promise_type : public PromiseBase
		{
			virtual ~promise_type() {}

			constexpr Task<T> get_return_object() noexcept { return Task<T>(std::coroutine_handle<promise_type>::from_promise(*this)); }

			constexpr void return_value(const T& val) noexcept { value.emplace(std::move(val)); }
			constexpr void return_value(T&& val) noexcept { value.emplace(val); }

			std::optional<T> value;
		};

		using Handle = std::coroutine_handle<promise_type>;

		Task(Handle handle) : handle(handle) {}
		~Task() {}

		auto operator co_await()
		{
			struct Awaiter
			{
				constexpr bool await_ready() const noexcept { return false; }

				constexpr T await_resume() const noexcept
				{
					return coro.promise().value.value();
				}

				void await_suspend(std::coroutine_handle<> handle)
				{
					auto a = PromiseBase::cast(handle);
					coro.promise().awaiting_coro = a;
					a.promise().scheduler->schedule(coro);
				}

				std::coroutine_handle<promise_type> coro;

				Awaiter(std::coroutine_handle<promise_type> coro) : coro(coro) {}
			};

			return Awaiter(handle);
		}

		Handle handle;
	};





	template<>
	class Task<void>
	{
	public:
		struct promise_type : public PromiseBase
		{
			virtual ~promise_type() {}

			Task<void> get_return_object() noexcept { return Task<void>(std::coroutine_handle<promise_type>::from_promise(*this)); }

			constexpr void return_void() const noexcept {}
		};

		using Handle = std::coroutine_handle<promise_type>;

		Task(Handle handle) : handle(handle) {}
		~Task() {}

		auto operator co_await()
		{
			struct Awaiter
			{
				constexpr bool await_ready() const noexcept { return false; }
				constexpr void await_resume() const noexcept {}

				void await_suspend(std::coroutine_handle<> handle)
				{
					auto a = PromiseBase::cast(handle);
					coro.promise().awaiting_coro = a;
					a.promise().scheduler->schedule(coro);
				}

				std::coroutine_handle<promise_type> coro;

				Awaiter(std::coroutine_handle<promise_type> coro) : coro(coro) {}
			};

			return Awaiter(handle);
		}

		Handle handle;
	};
}
