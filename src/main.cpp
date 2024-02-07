#include "pch.hpp"
#include "tasky.hpp"

using namespace tasky;

Task<int> test2(int i)
{
	co_return i + 1;
}

Task<void> test3(int i)
{
	if (i == 0)
		throw std::runtime_error("i == 0");

	std::cout << i << std::endl;

	co_return;
}

Task<void> test(int loops = 1)
{
	// await till the loop is done
	auto y = co_await tasky::all({ test2(0), test2(1), test2(2) });

	for (auto& x : y)
		std::cout << x << std::endl;

	co_await tasky::all({ test3(0), test3(1), test3(2) });
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
	int loops = argc > 1 ? atoi(argv[1]) : 1;

	Scheduler s;

	// schedule the test coroutine to run
	s.schedule(test(loops));

	try
	{
		// run all schedules tasks/coroutines
		s.run();
	}
	catch (const std::runtime_error& e)
	{
		std::cout << e.what() << std::endl;
	}

	return 0;
}
