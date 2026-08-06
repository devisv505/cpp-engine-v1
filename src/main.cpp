#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "core/Application.h"

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;
    return engine::Application{}.Run();
}
