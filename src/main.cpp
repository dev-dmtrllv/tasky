#include "pch.hpp"
#include "tasky.hpp"

using namespace tasky;

Task<int> add(int a, int b)
{
	co_return a + b;
}

Task<int> test_loop(int loops)
{
	int x = 0;

	for (int i = 0; i < loops; i++)
		x += co_await add(x, i); // await till the add is done

	co_return x;
}

Task<void> test(int loops = 1)
{
	// await till the loop is done
	auto x = co_await test_loop(loops);
	std::cout << x << std::endl;
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
	Scheduler s;
	
	// schedule the test coroutine to run
	s.schedule(test(1000));

	// run all schedules tasks/coroutines
	s.run();

	return 0;
}
