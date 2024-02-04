#include "pch.hpp"

#define LOG(...) std::println(__VA_ARGS__)
#define DBG(...) // std::println(__VA_ARGS__)

class Scheduler;
class TaskBasePromise;

using BaseHandle = std::coroutine_handle<TaskBasePromise>;

template<typename T>
class Task;

class Scheduler
{
public:
	Scheduler() {}
	~Scheduler() {}

	template<typename T>
	void schedule(Task<T>&& task)
	{
		DBG("schedule");
		schedule(task.handle.address());
	}

	template<typename T, typename... Ts>
	void schedule(Task<T>&& task, Ts&&... ts)
	{
		schedule(task);
		schedule(std::forward<Ts>(ts)...);
	}

	void run();

	void schedule(void* taskPtr);

	std::queue<void*> tasks;
};


class TaskBasePromise
{
public:
	static void* operator new(std::size_t bytes)
	{
		LOG("alloc");
		return malloc(bytes);
	}

	static void operator delete(void* ptr)
	{
		LOG("free");
		free(ptr);
	}

	template<typename T>
	static BaseHandle castHandle(std::coroutine_handle<T> handle)
	{
		return BaseHandle::from_address(handle.address());
	}

	static TaskBasePromise& from(void* ptr)
	{
		return BaseHandle::from_address(ptr).promise();
	}

	virtual ~TaskBasePromise()
	{
		LOG("~TaskBasePromise");
	}

	std::suspend_always initial_suspend() noexcept
	{
		DBG("initial_suspend()");
		return {};
	}

	std::suspend_always final_suspend() noexcept
	{
		DBG("final_suspend()");
		return {};
	}

	void unhandled_exception()
	{
		DBG("return_value()");
		exception.emplace(std::current_exception());
	}

	std::optional<std::exception_ptr> exception = {};
	Scheduler* scheduler = nullptr;
	BaseHandle awaitingHandle = nullptr;
};

class TaskBase
{
public:
	using promise_type = TaskBasePromise;

	template<typename T>
	TaskBase(std::coroutine_handle<T> handle) : handle(BaseHandle::from_address(handle.address())) {}
	virtual ~TaskBase() { DBG("~TaskBase()"); }

	template<typename T>
	inline T& promise() const noexcept { return std::coroutine_handle<T>::from_address(handle.address()).promise(); }

	bool await_ready()
	{
		DBG("await_ready() -> false");
		return false;
	}

	template<typename T>
	void await_suspend([[maybe_unused]] std::coroutine_handle<T> parentHandle)
	{
		DBG("await_suspend()");
		auto& p = handle.promise();
		p.awaitingHandle = TaskBasePromise::castHandle(parentHandle);
		p.scheduler = p.awaitingHandle.promise().scheduler;
		p.scheduler->schedule(this->handle.address());
	}

	BaseHandle handle;
};

template<typename T>
class Task : public TaskBase
{
public:
	class promise_type : public TaskBasePromise
	{
	public:
		virtual ~promise_type() {}

		Task<T> get_return_object()
		{
			DBG("get_return_object()");
			return Task(std::coroutine_handle<promise_type>::from_promise(*this));
		}

		void return_value(T val)
		{
			DBG("return_value()");
			this->value.emplace(val);
		}

		std::optional<T> value = {};
	};

	Task(std::coroutine_handle<promise_type> handle) : TaskBase(handle) {}
	virtual ~Task() {}

	T await_resume()
	{
		DBG("await_resume()");
		return promise<promise_type>().value.value();
	}
};

Task<int> test2(int a, int b)
{
	LOG("test2({}, {}) -> {}", a, b, a + b);
	co_return a + b;
}

Task<int> test(int count)
{
	int a = 0;

	for (int i = 0; i < count; i++)
		a = co_await test2(a, i);

	LOG("test result: {}", a);

	co_return a;
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
	int c = argc == 2 ? atoi(argv[1]) : 1;

	Scheduler s;

	s.schedule(test(c));
	s.run();

	return 0;
}

void Scheduler::run()
{
	while (tasks.size() > 0)
	{
		auto task = BaseHandle::from_address(tasks.front());
		tasks.pop();

		if (task.done())
		{
			DBG("Task already done!");
		}
		else
		{
			task.resume();
			if (task.done())
			{
				if (task.promise().awaitingHandle != nullptr)
				{
					schedule(task.promise().awaitingHandle.address());
				}

				task.destroy();
			}
		}
	}
}

void Scheduler::schedule(void* taskPtr)
{
	BaseHandle::from_address(taskPtr).promise().scheduler = this;
	tasks.push(taskPtr);
}
