#include "pch.hpp"

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
	template<typename T>
	void schedule(std::coroutine_handle<T> handle)
	{
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
		while (queue_.size() > 0)
		{
			auto handle = queue_.front();
			queue_.pop();

			if (!handle.done())
			{
				handle.resume();
				if (handle.done())
				{
					auto awaiting = handle.promise().awaiting_coro;
					if (awaiting != nullptr)
						queue_.push(handle.promise().awaiting_coro);
				}
			}
		}
	}

private:
	std::queue<std::coroutine_handle<PromiseBase>> queue_;
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

Task<int> add(int a, int b)
{
	std::cout << "add " << a << " + " << b << std::endl;
	co_return a + b;
}

Task<int> test_loop(int loops)
{
	int x = 0;

	for (int i = 0; i < loops; i++)
		x += co_await add(x, i);

	co_return x;
}

Task<void> test(int loops = 1)
{
	std::cout << co_await test_loop(loops) << std::endl;
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
	Scheduler s;

	s.schedule(test(10), test(10), test(20));

	s.run();

	return 0;
}
