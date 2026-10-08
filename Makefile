PYTHON ?= python3
BUILD ?= build
CXX ?= g++

.PHONY: cpp python test example

cpp:
	cmake -S . -B $(BUILD) -DTALLY_BUILD_TESTS=ON -DTALLY_BUILD_PYTHON=OFF -DCMAKE_CXX_COMPILER=$(CXX)
	cmake --build $(BUILD) -j
	ctest --test-dir $(BUILD) --output-on-failure

python:
	$(PYTHON) -m pip install -e ".[dev]"
	$(PYTHON) -m pytest

test: cpp python

example: cpp
	$(BUILD)/tally_example tests/data/words.txt
