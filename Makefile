CC = g++
CFLAGS = -std=c++17
LDFLAGS = -lSDL2 -lSDL2_image -lSDL2_ttf

task_manager: task_manager.cpp
	$(CC) $(CFLAGS) -o task_manager task_manager.cpp $(LDFLAGS)

clean:
	rm -f task_manager