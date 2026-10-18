#pragma once

#include "bending/application/FemExperiment.h"
#include <memory>

namespace bending::fem {
class DockerFemRunner final : public application::FemRunner {
  public:
    DockerFemRunner(std::filesystem::path scripts, std::filesystem::path results,
                    std::filesystem::path dockerExecutable = "docker");
    ~DockerFemRunner() override;
    void start(application::FemRequest request, Progress progress, Completion completion) override;
    void cancel() override;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace bending::fem
