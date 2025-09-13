#pragma once

#include <workerd/api/immediate-crash.h>
#include <workerd/jsg/jsg.h>

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>

#define SHM_SIZE 0x200000
#define MAX_EDGES ((SHM_SIZE - 4) * 8)

struct shmem_data {
  uint32_t num_edges;
  unsigned char edges[];
};

// NOLINTBEGIN(edgeworker-mutable-globals)
extern struct shmem_data* __shmem;
extern uint32_t* __edges_start;
extern uint32_t* __edges_stop;
// NOLINTEND(edgeworker-mutable-globals)

void __sanitizer_cov_reset_edgeguards();

//Fuzzilli sockets for communication
#define REPRL_CRFD 100
#define REPRL_CWFD 101
#define REPRL_DRFD 102
#define REPRL_DWFD 103
#define USE(...)                                                                                   \
  do {                                                                                             \
    (void)(__VA_ARGS__);                                                                           \
  } while (false)

#define CHECK(condition)                                                                           \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      fprintf(stderr, "Error: %s:%d: condition failed: %s\n", __FILE__, __LINE__, #condition);     \
      exit(EXIT_FAILURE);                                                                          \
    }                                                                                              \
  } while (0)

void perform_wild_write();
void sanitizer_cov_reset_edgeguards();
uint32_t sanitizer_cov_count_discovered_edges();
void sanitizer_cov_prepare_for_hardware_sandbox();
void cov_init_builtins_edges(uint32_t num_edges);

void fuzzilli_handler(workerd::jsg::Lock& js, workerd::jsg::Arguments<workerd::jsg::Value>& args);

// TODO: this would only work with the profiler like in d8
// void cov_update_builtins_basic_block_coverage(const std::vector<bool>& cov_map);
// https://github.com/v8/v8/blob/a63b49495eac716c1a4ccbef57e2e1c7e98f27e4/src/d8/d8.cc#L7284-L7286
//
