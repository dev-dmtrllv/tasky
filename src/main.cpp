#include "pch.hpp"
#include "tasky.hpp"

using namespace tasky;

Task<void> read()
{
	auto s = co_await all({
		readFile("C:\\Users\\lilov\\Desktop\\test.txt"),
		readFile("C:\\Users\\lilov\\Desktop\\test2.txt"),
		readFile("C:\\Users\\lilov\\Desktop\\test3.txt")
	});

	for (const auto& str : s)
		std::cout << str.c_str() << std::endl;
}

Task<int> async_main()
{
	co_await read();
	co_await writeFile("C:\\Users\\lilov\\Desktop\\test2.txt", "WOPS");
	co_await read();
	co_return 0;
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
	Scheduler s;

	try
	{
		auto main_task = async_main();
		s.schedule(main_task);
		
		auto begin = std::chrono::steady_clock::now();
		s.run();
		auto end = std::chrono::steady_clock::now();
		
		auto s = std::chrono::duration_cast<std::chrono::milliseconds>(end - begin);
		std::cout << s.count() << "ms" << std::endl;
		return 0;
	}
	catch (const std::runtime_error& e)
	{
		std::cerr << "Exception: " << e.what() << std::endl;
		return 1;
	}
}
