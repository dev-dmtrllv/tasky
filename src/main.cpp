#include "pch.hpp"
#include "tasky.hpp"

using namespace tasky;

Task<std::size_t> add(std::size_t a, std::size_t b)
{
	std::cout << std::this_thread::get_id() << std::endl;
	co_return a + b;
}

Task<std::size_t> test_loop(std::size_t loops)
{
	std::size_t x = 0;

	for (std::size_t i = 0; i < loops; i++)
		x += co_await add(x, i);

	co_return x;
}

Task<void> test(std::size_t loops = 1)
{
	std::cout << co_await test_loop(loops) << std::endl;
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
	Scheduler s;

	s.schedule(test(1000));

	s.run();

	return 0;
}
