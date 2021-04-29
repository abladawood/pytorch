#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include <unistd.h>

#include <c10d/ProcessGroupMPI.hpp>

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

// Wait for work to complete
void waitWork(
    c10::intrusive_ptr<::c10d::ProcessGroupMPI> pg,
    std::vector<c10::intrusive_ptr<c10d::ProcessGroup::Work>> works,
    std::vector<std::vector<at::Tensor>>* outputTensors) {
  for (auto& work : works) {
    try {
      work->wait();
    } catch (const std::exception& ex) {
      std::cerr << "Exception received: " << ex.what() << std::endl;
      pg->abort();
    }
    if (outputTensors) {
      auto outputs = work->result();
      outputTensors->emplace_back(outputs);
    }
  }
}

void testAllreduce(int iter = 1000) {
  auto pg = c10d::ProcessGroupMPI::createProcessGroupMPI();

  // Generate inputs
  std::vector<std::vector<at::Tensor>> allTensors(iter);
  for (auto i = 0; i < iter; ++i) {
    auto tensor = at::ones({16, 16}) * i;
    allTensors[i] = std::vector<at::Tensor>({tensor});
  }

  std::vector<c10::intrusive_ptr<::c10d::ProcessGroup::Work>> works;
  for (auto& tensors : allTensors) {
    // Queue the work.
    c10::intrusive_ptr<::c10d::ProcessGroup::Work> work =
        pg->allreduce(tensors);
    works.push_back(std::move(work));
  }

  std::vector<std::vector<at::Tensor>> outputTensors;
  waitWork(pg, works, &outputTensors);

  // Get the world size
  auto worldSize = pg->getSize();

  // Verify outputs
  for (int i = 0; i < iter; ++i) {
    const auto expected = worldSize * i;
    auto data = outputTensors[i][0].data_ptr<float>();
    for (auto j = 0; j < outputTensors[i][0].numel(); ++j) {
      if (data[j] != expected) {
        throw std::runtime_error("BOOM!");
      }
    }
  }
}

void testBroadcast(int iter = 10000) {
  auto pg = c10d::ProcessGroupMPI::createProcessGroupMPI();
  // Generate inputs
  std::vector<std::vector<at::Tensor>> allTensors(iter);

  for (auto i = 0; i < iter; ++i) {
    if (pg->getRank() == 0) {
      auto tensor = at::ones({16, 16}) * i;
      allTensors[i] = std::vector<at::Tensor>({tensor});
    } else {
      auto tensor = at::zeros({16, 16});
      allTensors[i] = std::vector<at::Tensor>({tensor});
    }
  }

  std::vector<c10::intrusive_ptr<::c10d::ProcessGroup::Work>> works;
  for (auto& tensors : allTensors) {
    // Queue the work.
    c10::intrusive_ptr<::c10d::ProcessGroup::Work> work =
        pg->broadcast(tensors);
    works.push_back(std::move(work));
  }

  std::vector<std::vector<at::Tensor>> outputTensors;

  waitWork(pg, works, &outputTensors);

  // Verify outputs
  for (int i = 0; i < iter; ++i) {
    const auto expected = i;
    auto data = outputTensors[i][0].data_ptr<float>();
    for (auto j = 0; j < outputTensors[i][0].numel(); ++j) {
      if (data[j] != expected) {
        throw std::runtime_error("BOOM!");
      }
    }
  }
}

void testReduce(int iter = 10000) {
  auto pg = c10d::ProcessGroupMPI::createProcessGroupMPI();
  // Generate inputs
  std::vector<std::vector<at::Tensor>> allTensors(iter);

  for (auto i = 0; i < iter; ++i) {
    auto tensor = at::ones({16, 16}) * i;
    allTensors[i] = std::vector<at::Tensor>({tensor});
  }

  std::vector<c10::intrusive_ptr<::c10d::ProcessGroup::Work>> works;
  for (auto& tensors : allTensors) {
    // Queue the work.
    c10::intrusive_ptr<::c10d::ProcessGroup::Work> work = pg->reduce(tensors);
    works.push_back(std::move(work));
  }

  std::vector<std::vector<at::Tensor>> outputTensors;

  waitWork(pg, works, &outputTensors);

  // Get the world size
  auto worldSize = pg->getSize();

  if (pg->getRank() == 0) {
    // Verify outputs
    for (int i = 0; i < iter; ++i) {
      const auto expected = worldSize * i;
      auto data = outputTensors[i][0].data_ptr<float>();
      for (auto j = 0; j < outputTensors[i][0].numel(); ++j) {
        if (data[j] != expected) {
          throw std::runtime_error("BOOM!");
        }
      }
    }
  }
}

void testAllgather(int iter = 10000) {
  auto pg = c10d::ProcessGroupMPI::createProcessGroupMPI();
  std::vector<std::vector<at::Tensor>> allTensors(iter);
  std::vector<std::vector<std::vector<at::Tensor>>> allOutputTensors(iter);

  // Get the world size
  auto worldSize = pg->getSize();
  auto rank = pg->getRank();

  // Generate inputs
  for (auto i = 0; i < iter; ++i) {
    auto tensor = at::ones({16, 16}) * i * rank;
    allTensors[i] = std::vector<at::Tensor>({tensor});
    allOutputTensors[i] = std::vector<std::vector<at::Tensor>>(1);
    allOutputTensors[i][0].resize(worldSize);
    for (auto j = 0; j < worldSize; ++j) {
      allOutputTensors[i][0][j] = at::zeros({16, 16});
    }
  }

  std::vector<c10::intrusive_ptr<::c10d::ProcessGroup::Work>> works;
  for (size_t i = 0; i < allTensors.size(); ++i) {
    // Queue the work.
    c10::intrusive_ptr<::c10d::ProcessGroup::Work> work =
        pg->allgather(allOutputTensors[i], allTensors[i]);
    works.push_back(std::move(work));
  }

  std::vector<std::vector<at::Tensor>> outputTensors;

  waitWork(pg, works, &outputTensors);

  // Verify outputs
  for (int i = 0; i < iter; ++i) {
    for (int j = 0; j < worldSize; ++j) {
      const auto expected = i * j;
      auto data = outputTensors[i][j].data_ptr<float>();
      for (auto k = 0; k < outputTensors[i][j].numel(); ++k) {
        if (data[k] != expected) {
          throw std::runtime_error("BOOM!");
        }
      }
    }
  }
}

void testGather(int iter = 1000) {
  auto pg = c10d::ProcessGroupMPI::createProcessGroupMPI();
  std::vector<std::vector<at::Tensor>> allTensors(iter);
  std::vector<std::vector<std::vector<at::Tensor>>> allOutputTensors(iter);

  // Get the world size
  auto worldSize = pg->getSize();
  auto rank = pg->getRank();

  // Generate inputs
  for (auto i = 0; i < iter; ++i) {
    auto tensor = at::ones({16, 16}) * i * rank;
    allTensors[i] = std::vector<at::Tensor>({tensor});
    if (rank == 0) {
      allOutputTensors[i] = std::vector<std::vector<at::Tensor>>(1);
      allOutputTensors[i][0].resize(worldSize);
      for (auto j = 0; j < worldSize; ++j) {
        allOutputTensors[i][0][j] = at::zeros({16, 16});
      }
    } else {
      allOutputTensors[i] = std::vector<std::vector<at::Tensor>>(0);
    }
  }

  std::vector<c10::intrusive_ptr<::c10d::ProcessGroup::Work>> works;
  for (size_t i = 0; i < allTensors.size(); ++i) {
    // Queue the work.
    c10::intrusive_ptr<::c10d::ProcessGroup::Work> work =
        pg->gather(allOutputTensors[i], allTensors[i]);
    works.push_back(std::move(work));
  }

  std::vector<std::vector<at::Tensor>> outputTensors;

  waitWork(pg, works, &outputTensors);

  // Verify outputs
  if (rank == 0) {
    for (int i = 0; i < iter; ++i) {
      for (int j = 0; j < worldSize; ++j) {
        const auto expected = i * j;
        auto data = outputTensors[i][j].data_ptr<float>();
        for (auto k = 0; k < outputTensors[i][j].numel(); ++k) {
          if (data[k] != expected) {
            throw std::runtime_error("BOOM!");
          }
        }
      }
    }
  } else {
    for (int i = 0; i < iter; ++i) {
      if (outputTensors[i].size() != 0) {
        throw std::runtime_error("BOOM!");
      }
    }
  }
}

void testScatter(int iter = 1) {
  auto pg = c10d::ProcessGroupMPI::createProcessGroupMPI();

  std::vector<std::vector<std::vector<at::Tensor>>> allInputTensors(iter);
  std::vector<std::vector<at::Tensor>> allTensors(iter);

  // Get the world size
  auto worldSize = pg->getSize();
  auto rank = pg->getRank();

  // Generate inputs
  for (auto i = 0; i < iter; ++i) {
    auto tensor = at::zeros({16, 16});
    allTensors[i] = std::vector<at::Tensor>({tensor});
    if (rank == 0) {
      allInputTensors[i] = std::vector<std::vector<at::Tensor>>(1);
      allInputTensors[i][0].resize(worldSize);
      for (auto j = 0; j < worldSize; ++j) {
        allInputTensors[i][0][j] = at::ones({16, 16}) * i * j;
      }
    } else {
      allInputTensors[i] = std::vector<std::vector<at::Tensor>>(0);
    }
  }

  std::vector<c10::intrusive_ptr<::c10d::ProcessGroup::Work>> works;
  for (size_t i = 0; i < allTensors.size(); ++i) {
    // Queue the work.
    c10::intrusive_ptr<::c10d::ProcessGroup::Work> work =
        pg->scatter(allTensors[i], allInputTensors[i]);
    works.push_back(std::move(work));
  }

  std::vector<std::vector<at::Tensor>> outputTensors;

  waitWork(pg, works, &outputTensors);

  // Verify outputs
  for (int i = 0; i < iter; ++i) {
    for (int j = 0; j < worldSize; ++j) {
      const auto expected = i * j;
      auto data = outputTensors[i][0].data_ptr<float>();
      for (auto k = 0; k < outputTensors[i][0].numel(); ++k) {
        if (data[k] != expected) {
          throw std::runtime_error("BOOM!");
        }
      }
    }
  }
}

void testSendRecv(bool recvAnysource, int iter = 10000) {
  auto pg = c10d::ProcessGroupMPI::createProcessGroupMPI();
  // Generate inputs
  std::vector<std::vector<at::Tensor>> allTensors(iter);
  auto rank = pg->getRank();
  for (auto i = 0; i < iter; ++i) {
    if (rank == 0) {
      auto tensor = at::ones({16, 16}) * i;
      allTensors[i] = std::vector<at::Tensor>({tensor});
    } else {
      auto tensor = at::zeros({16, 16});
      allTensors[i] = std::vector<at::Tensor>({tensor});
    }
  }

  if (rank == 0) {
    std::vector<c10::intrusive_ptr<::c10d::ProcessGroup::Work>> works;
    for (auto& tensors : allTensors) {
      // Queue the work.
      c10::intrusive_ptr<::c10d::ProcessGroup::Work> work =
          pg->send(tensors, 1, 0);
      works.push_back(std::move(work));
    }
    waitWork(pg, works, nullptr);
  } else if (rank == 1) {
    std::vector<c10::intrusive_ptr<::c10d::ProcessGroup::Work>> works;
    std::vector<int> srcRanks;
    size_t i = 0;
    for (auto& tensors : allTensors) {
      // Queue the work.
      if (!recvAnysource) {
        c10::intrusive_ptr<::c10d::ProcessGroup::Work> work =
            pg->recv(tensors, 0, 0);
        works.push_back(std::move(work));
      } else {
        c10::intrusive_ptr<::c10d::ProcessGroup::Work> work =
            pg->recvAnysource(tensors, 0);
        works.push_back(std::move(work));
      }
      ++i;
    }

    std::vector<std::vector<at::Tensor>> outputTensors;

    waitWork(pg, works, &outputTensors);

    for (const auto& work : works) {
      srcRanks.push_back(work->sourceRank());
    }

    // Verify outputs
    for (int i = 0; i < iter; ++i) {
      if (recvAnysource && srcRanks[i] != 0) {
        throw std::runtime_error("src rank is wrong for recvAnysource");
      }
      const auto expected = i;
      auto data = outputTensors[i][0].data_ptr<float>();
      for (auto j = 0; j < outputTensors[i][0].numel(); ++j) {
        if (data[j] != expected) {
          throw std::runtime_error("BOOM!");
        }
      }
    }
  }
}

void testBackendName() {
  auto pg = c10d::ProcessGroupMPI::createProcessGroupMPI();
  if (pg->getBackendName() != std::string(c10d::MPI_BACKEND_NAME)) {
    throw std::runtime_error("BOOM!");
  }
}

int main(int argc, char** argv) {
#ifdef MPIEXEC
  // If we are within an openmpi mpirun, then skip the exec
  if (!std::getenv("OMPI_COMM_WORLD_SIZE")) {
    std::cout << "Execute mpiexec from: " << STR(MPIEXEC) << std::endl;
    execl(STR(MPIEXEC), "-np 2", argv[0], (char*)nullptr);
  }

  testAllreduce();
  testBroadcast();
  testReduce();
  testAllgather();
  testGather();
  testScatter();
  testSendRecv(false);
  testSendRecv(true);
  testBackendName();

  std::cout << "Test successful" << std::endl;
#else
  std::cout << "MPI executable not found, skipping test" << std::endl;
#endif
  return EXIT_SUCCESS;
}
