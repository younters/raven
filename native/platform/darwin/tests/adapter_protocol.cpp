#include "cpu/native_cpu.h"
#include <fstream>
#include <iostream>

int main(int argc, char **argv)
{
	if (argc != 6) return 2;
	try {
		std::ifstream file(argv[2], std::ios::binary);
		std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
		raven::PEImage image(bytes);
		raven::LoadedImage loaded(image, image.image_base);
		raven::LoadedImage *images[] = {&loaded};
		raven::NativeProcess process(images, argv[1], std::chrono::seconds(5));
		for (int index = 3; index < 5; ++index) {
			bool rejected = false;
			try { (void)process.load_unix_library(argv[index], 132); }
			catch (const std::exception &) { rejected = true; }
			if (!rejected) throw std::runtime_error("incompatible adapter was loaded");
		}
		if (!process.load_unix_library(argv[5], 132)) throw std::runtime_error("matching adapter was rejected");
		std::cout << "missing and v4 adapter versions rejected before init; matching adapter loaded\n";
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
