# chiaki-tizen WASM module
#
# Build with the *Samsung* Emscripten SDK (their fork adds the Tizen Sockets
# Extension and the EMSS/WASM Player C++ API — a vanilla emsdk will not link):
#
#   source /path/to/samsung-emsdk/emsdk_env.sh
#   emcmake cmake -B build -G Ninja -DCHIAKI_NG_ROOT=/path/to/chiaki-ng
#   ninja -C build
#
# Produces chiaki-tizen.{js,wasm} to be placed in the Tizen web app's
# wasm_modules/ directory.

cmake_minimum_required(VERSION 3.20)
project(chiaki-tizen C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

if(NOT DEFINED CHIAKI_NG_ROOT)
	get_filename_component(CHIAKI_NG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../chiaki-ng" ABSOLUTE)
endif()
message(STATUS "chiaki-ng at: ${CHIAKI_NG_ROOT}")

# ---------------------------------------------------------------------------
# Third-party: mbedTLS (crypto) and Opus, fetched from GitHub.
# mbedTLS is used instead of OpenSSL because it cross-compiles cleanly under
# Emscripten; chiaki has first-class mbedTLS support (CHIAKI_LIB_ENABLE_MBEDTLS).
# ---------------------------------------------------------------------------
include(FetchContent)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(mbedtls
	GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
	# chiaki-ng's mbedTLS backend accesses the v2 ECDH context fields.
	GIT_TAG v2.28.0
	GIT_SHALLOW TRUE)

set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_STACK_PROTECTOR OFF CACHE BOOL "" FORCE)
FetchContent_Declare(opus
	GIT_REPOSITORY https://github.com/xiph/opus.git
	GIT_TAG v1.5.2
	GIT_SHALLOW TRUE)

FetchContent_MakeAvailable(mbedtls opus)

file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/opus-compat)
execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink
	${opus_SOURCE_DIR}/include
	${CMAKE_CURRENT_BINARY_DIR}/opus-compat/opus)

# ---------------------------------------------------------------------------
# Vendored third-party from the chiaki-ng tree (plain C, compiles anywhere)
# ---------------------------------------------------------------------------
set(NANOPB_DIR ${CHIAKI_NG_ROOT}/third-party/nanopb)
set(JERASURE_DIR ${CHIAKI_NG_ROOT}/third-party/jerasure)
set(GF_DIR ${CHIAKI_NG_ROOT}/third-party/gf-complete)

add_library(nanopb-runtime STATIC
	${NANOPB_DIR}/pb_common.c
	${NANOPB_DIR}/pb_decode.c
	${NANOPB_DIR}/pb_encode.c)
target_include_directories(nanopb-runtime PUBLIC ${NANOPB_DIR})

file(GLOB GF_SOURCES ${GF_DIR}/src/*.c)
file(GLOB JERASURE_SOURCES ${JERASURE_DIR}/src/*.c)
add_library(jerasure STATIC ${GF_SOURCES} ${JERASURE_SOURCES})
target_include_directories(jerasure PUBLIC
	${JERASURE_DIR}/include ${GF_DIR}/include)
# No SIMD intrinsics under wasm32
target_compile_definitions(jerasure PRIVATE ARCH_64=0)

# ---------------------------------------------------------------------------
# chiaki core (local-network subset: remote/holepunch still stubbed below).
# RUDP (remote/rudp.c + rudpsendbuffer.c) is compiled from real chiaki-ng —
# it is a self-contained reliable-UDP layer over sockets with no libcurl/
# libevent/json-c dependency, unlike holepunch.c. It is inert until a
# holepunch session hands it a socket (connect_info.rudp_sock stays NULL on
# the local-network path), so this is a no-op for local play and the first
# building block for over-the-internet play.
# ---------------------------------------------------------------------------
set(CHIAKI_LIB_SRC ${CHIAKI_NG_ROOT}/lib/src)
set(CHIAKI_CORE_SOURCES
	${CHIAKI_LIB_SRC}/common.c
	${CHIAKI_LIB_SRC}/sock.c
	${CHIAKI_LIB_SRC}/session.c
	${CHIAKI_LIB_SRC}/thread.c
	${CHIAKI_LIB_SRC}/base64.c
	${CHIAKI_LIB_SRC}/http.c
	${CHIAKI_LIB_SRC}/log.c
	${CHIAKI_LIB_SRC}/ctrl.c
	${CHIAKI_LIB_SRC}/rpcrypt.c
	${CHIAKI_LIB_SRC}/takion.c
	${CHIAKI_LIB_SRC}/senkusha.c
	${CHIAKI_LIB_SRC}/streamconnection.c
	${CHIAKI_LIB_SRC}/ecdh.c
	${CHIAKI_LIB_SRC}/launchspec.c
	${CHIAKI_LIB_SRC}/random.c
	${CHIAKI_LIB_SRC}/gkcrypt.c
	${CHIAKI_LIB_SRC}/audio.c
	${CHIAKI_LIB_SRC}/audioreceiver.c
	${CHIAKI_LIB_SRC}/audiosender.c
	${CHIAKI_LIB_SRC}/videoreceiver.c
	${CHIAKI_LIB_SRC}/frameprocessor.c
	${CHIAKI_LIB_SRC}/packetstats.c
	${CHIAKI_LIB_SRC}/discovery.c
	${CHIAKI_LIB_SRC}/congestioncontrol.c
	${CHIAKI_LIB_SRC}/stoppipe.c
	${CHIAKI_LIB_SRC}/reorderqueue.c
	${CHIAKI_LIB_SRC}/discoveryservice.c
	${CHIAKI_LIB_SRC}/feedback.c
	${CHIAKI_LIB_SRC}/feedbacksender.c
	${CHIAKI_LIB_SRC}/controller.c
	${CHIAKI_LIB_SRC}/takionsendbuffer.c
	${CHIAKI_LIB_SRC}/time.c
	${CHIAKI_LIB_SRC}/fec.c
	${CHIAKI_LIB_SRC}/regist.c
	${CHIAKI_LIB_SRC}/opusdecoder.c
	${CHIAKI_LIB_SRC}/opusencoder.c
	${CHIAKI_LIB_SRC}/orientation.c
	${CHIAKI_LIB_SRC}/bitstream.c
	# reliable-UDP transport for remote play (no heavy deps; inert until
	# a holepunch session provides a socket — see note above)
	${CHIAKI_LIB_SRC}/remote/rudp.c
	${CHIAKI_LIB_SRC}/remote/rudpsendbuffer.c
	# vendored generated protobuf (no protoc needed at build time)
	${CMAKE_CURRENT_SOURCE_DIR}/gen/takion.pb.c)

add_library(chiaki-core STATIC ${CHIAKI_CORE_SOURCES})
target_include_directories(chiaki-core PUBLIC
	${CHIAKI_NG_ROOT}/lib/include
	${CMAKE_CURRENT_SOURCE_DIR}/gen  # chiaki/config.h + takion.pb.h
	${CMAKE_CURRENT_SOURCE_DIR}/src
	${CMAKE_CURRENT_BINARY_DIR}/opus-compat)
target_link_libraries(chiaki-core PUBLIC
	nanopb-runtime jerasure opus
	mbedtls mbedcrypto mbedx509)
target_compile_definitions(chiaki-core PUBLIC
	CHIAKI_TIZEN_WASM=1
	CHIAKI_LIB_ENABLE_MBEDTLS=1
	CHIAKI_ENABLE_PSN_LOGIN=0)
# Force the Tizen socket compat shim into every chiaki translation unit
target_compile_options(chiaki-core PRIVATE
	-include ${CMAKE_CURRENT_SOURCE_DIR}/src/tizen_compat.h
	-pthread)

# ---------------------------------------------------------------------------
# The Tizen module: bridge + EMSS player + holepunch stubs
# ---------------------------------------------------------------------------
add_executable(chiaki-tizen
	src/bridge.cc
	src/emss_player.cc
	# Real holepunch adapter (replaces holepunch_stub.c). Provides the same
	# holepunch API surface backed by sockets the WASM transport punches; the
	# PSN signaling runs in JS. Local play is unaffected (holepunch_session is
	# only set on the remote path).
	src/holepunch_bridge.c
	# Over-the-internet transport (promoted from the verified wasm/spikes/):
	# authenticated push WebSocket (mbedTLS + framing), STUN, UDP hole-punch.
	src/net/ws_client.c
	src/net/stun.c
	src/net/punch.c)
target_link_libraries(chiaki-tizen PRIVATE chiaki-core)
target_compile_options(chiaki-tizen PRIVATE -pthread)

if(EMSCRIPTEN)
	# PTHREAD_POOL_SIZE: chiaki spawns session, ctrl, stream-connection,
	# takion (x2: stream + senkusha phases), audio, video, discovery,
	# feedback-sender and regist threads; PROXY_TO_PTHREAD takes one more.
	# 16 leaves comfortable headroom — threads cannot be spawned lazily on
	# Tizen once the pool is exhausted.
	target_link_options(chiaki-tizen PRIVATE
		"SHELL:-s ENVIRONMENT_MAY_BE_TIZEN"
		"SHELL:-s USE_PTHREADS=1"
		"SHELL:-s PTHREAD_POOL_SIZE=16"
		"SHELL:-s PROXY_TO_PTHREAD"
		# TEMPORARY: default is 64KB. If a worker's thread-local storage
		# block (mbedTLS/opus context structs, chiaki's own thread-local
		# state) doesn't fit in that on thread startup, Emscripten's stack
		# overflow guard traps as a bare "unreachable" before main() ever
		# runs — which matches a crash with zero application-level
		# checkpoints reached. Remove/shrink once diagnosed.
		"SHELL:-s DEFAULT_PTHREAD_STACK_SIZE=1048576"
		"SHELL:-s ALLOW_MEMORY_GROWTH=1"
		"SHELL:-s TOTAL_MEMORY=134217728"
		"SHELL:-s WASM_MEM_MAX=536870912"
		"SHELL:-s EXPORTED_RUNTIME_METHODS=['ccall','cwrap']"
		"SHELL:-s EXIT_RUNTIME=0"
		# TEMPORARY: turns bare "unreachable" traps (uncaught C++ exceptions,
		# null function pointer calls, stack overflow, etc.) into a
		# descriptive abort() message instead of a silent trap. Costs some
		# code size/perf — drop back to ASSERTIONS=0 (or remove) once the
		# current worker-thread crash is diagnosed.
		"SHELL:-s ASSERTIONS=2"
		# TEMPORARY: ASSERTIONS alone doesn't help for a raw wasm-level
		# "unreachable" trap coming from inside compiled C++ (the trap
		# carries no message at the wasm level) — the RuntimeError's stack
		# just shows anonymous wasm-function[N] frames without this. Keeps
		# function names in the binary so the trap's stack trace is
		# symbolized. Remove once diagnosed.
		--profiling-funcs
		-pthread
		--bind)
	set_target_properties(chiaki-tizen PROPERTIES SUFFIX ".js")
endif()
