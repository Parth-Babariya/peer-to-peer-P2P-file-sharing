# Compiler to use
CXX = g++

# Compiler flags:
# -std=c++17: Use the C++17 standard
# -I../common: Shared code (framing, hashing) used by both programs
# -pthread: Enable support for POSIX threads
# -Wall -Wextra: Enable all common and extra warnings for better code quality
# -g: Include debugging information in the executable
CXXFLAGS = -std=c++17 -I../common -pthread -Wall -Wextra -g

# Linker flags:
# -lssl -lcrypto: Link against the OpenSSL libraries for SHA1 hashing functions
LDFLAGS = -lssl -lcrypto

# The final executable name
TARGET = tracker

# All source files that need to be compiled
SOURCES = tracker.cpp ../common/utils.cpp

# Object files are derived from source files (e.g., tracker.cpp -> tracker.o)
OBJECTS = $(SOURCES:.cpp=.o)

# The default rule, executed when you just run "make"
all: $(TARGET)

# Rule to link the final executable from the object files
$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(OBJECTS) $(LDFLAGS)

# Rule to compile a .cpp source file into a .o object file
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Rule to clean up the directory by removing the executable and object files
clean:
	rm -f $(TARGET) $(OBJECTS)