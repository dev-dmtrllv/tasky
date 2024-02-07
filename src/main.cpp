#include "pch.hpp"
#include "tasky.hpp"

using namespace tasky;

Task<int> async_main()
{
	std::cout << "Hello" << std::endl;
	
	co_return 0;
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
	Scheduler s;
	try
	{
		auto main_task = async_main();
		s.schedule(main_task);
		s.run();
		return 0;
	}
	catch (const std::runtime_error& e)
	{
		std::cerr << "Exception: " << e.what() << std::endl;
		return 1;
	}
}
