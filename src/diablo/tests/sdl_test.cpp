#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <unistd.h>

#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); std::exit(1); } } while (0)

int main()
{
	char data[] = "abcdef";
	SDL_RWops *rw = SDL_RWFromMem(data, 6);
	CHECK(rw != nullptr);
	CHECK(SDL_RWwrite(rw, "X", 0, 1) == 0);
	CHECK(SDL_RWtell(rw) == 0);
	CHECK(SDL_RWwrite(rw, "XY", 2, 1) == 1);
	CHECK(SDL_RWtell(rw) == 2);
	CHECK(SDL_RWseek(rw, std::numeric_limits<Sint64>::max(), RW_SEEK_CUR) == 6);
	CHECK(SDL_RWseek(rw, std::numeric_limits<Sint64>::min(), RW_SEEK_CUR) == 0);
	CHECK(SDL_RWseek(rw, -2, RW_SEEK_END) == 4);
	CHECK(SDL_RWseek(rw, 0, 999) == -1);
	CHECK(SDL_RWtell(rw) == 4);
	CHECK(SDL_RWwrite(rw, "1234", 2, 2) == 1);
	CHECK(std::memcmp(data, "XYcd12", 6) == 0);
	SDL_RWclose(rw);

	CHECK(SDL_RWFromMem(nullptr, 6) == nullptr);
	CHECK(SDL_RWFromMem(data, -1) == nullptr);
	CHECK(SDL_RWFromMem(data, 0) == nullptr);
	rw = SDL_RWFromConstMem(data, 6);
	CHECK(rw != nullptr);
	CHECK(SDL_RWwrite(rw, "!", 1, 1) == 0);
	CHECK(SDL_RWtell(rw) == 0);
	CHECK(data[0] == 'X');
	char read[6] {};
	CHECK(SDL_RWread(rw, read, 2, 3) == 3);
	CHECK(std::memcmp(read, data, 6) == 0);
	SDL_RWclose(rw);

	char path[] = "/tmp/diablo-rwops-XXXXXX";
	const int fd = mkstemp(path);
	CHECK(fd >= 0);
	CHECK(write(fd, "abc", 3) == 3);
	close(fd);
	rw = SDL_RWFromFile(path, "rb");
	unlink(path);
	CHECK(rw != nullptr);
	CHECK(SDL_RWseek(rw, 1, RW_SEEK_SET) == 1);
	CHECK(SDL_RWseek(rw, -1, RW_SEEK_SET) == -1);
	CHECK(SDL_RWtell(rw) == 1);
	SDL_RWclose(rw);
	std::puts("PASS: SDL memory streams, read-only buffers, and failed file seeks");
}
