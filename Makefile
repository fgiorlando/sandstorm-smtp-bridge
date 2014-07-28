# Sandstorm - Personal Cloud Sandbox
# Copyright (c) 2014 Sandstorm Development Group, Inc. and contributors
# All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# You may override the following vars on the command line to suit
# your config.
CXX=clang++
CXXFLAGS=-O2 -Wall

# You generally should not modify these.
CXXFLAGS2=-std=c++1y -Isrc -Itmp $(CXXFLAGS)

.PHONY: all clean

all: bin/test-smtp

clean:
	rm -rf bin tmp
bin/test-smtp: tmp/genfiles src/sandstorm/test-smtp.c++ src/sandstorm/sandstorm-smtp-bridge.h
	@echo "building bin/test-smtp..."
	@mkdir -p bin
	@$(CXX) src/sandstorm/test-smtp.c++ tmp/sandstorm/*.capnp.c++ -o bin/test-smtp -static $(CXXFLAGS2) `pkg-config gmime-2.6 capnp-rpc --static --cflags --libs`

tmp/genfiles: /opt/sandstorm/latest/usr/include/sandstorm/*.capnp
	@echo "generating capnp files..."
	@mkdir -p tmp
	@capnp compile --src-prefix=/opt/sandstorm/latest/usr/include -oc++:tmp  /opt/sandstorm/latest/usr/include/sandstorm/*.capnp
	@touch tmp/genfiles