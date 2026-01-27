# Compiler to use
CXX = g++

# Compiler flags:
# -std=c++11: Use the C++11 standard
# -pthread: Enable support for POSIX threads
# -Wall -Wextra: Enable all common and extra warnings for better code quality
# -g: Include debugging information in the executable
CXXFLAGS = -std=c++11 -pthread -Wall -Wextra -g

# Linker flags:
# -lssl -lcrypto: Link against the OpenSSL libraries for SHA1 hashing functions
LDFLAGS = -lssl -lcrypto

# The final executable name
TARGET = client

# All source files that need to be compiled
SOURCES = client.cpp utils.cpp

# Object files are derived from source files (e.g., client.cpp -> client.o)
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