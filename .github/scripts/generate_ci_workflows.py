#!/usr/bin/env python3

from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Dict, Set, List, Iterable

import jinja2
import json
import os
import sys
from typing_extensions import Literal

YamlShellBool = Literal["''", 1]
Arch = Literal["windows", "linux"]

DOCKER_REGISTRY = "308535385114.dkr.ecr.us-east-1.amazonaws.com"
GITHUB_DIR = Path(__file__).resolve().parent.parent

WINDOWS_CPU_TEST_RUNNER = "windows.4xlarge"
# contains 1 gpu
WINDOWS_CUDA_TEST_RUNNER = "windows.8xlarge.nvidia.gpu"
WINDOWS_RUNNERS = {
    WINDOWS_CPU_TEST_RUNNER,
    WINDOWS_CUDA_TEST_RUNNER,
}

LINUX_CPU_TEST_RUNNER = "linux.2xlarge"
# contains 1 gpu
LINUX_CUDA_TEST_RUNNER = "linux.4xlarge.nvidia.gpu"
LINUX_RUNNERS = {
    LINUX_CPU_TEST_RUNNER,
    LINUX_CUDA_TEST_RUNNER,
}

CUDA_RUNNERS = {
    WINDOWS_CUDA_TEST_RUNNER,
    LINUX_CUDA_TEST_RUNNER,
}
CPU_RUNNERS = {
    WINDOWS_CPU_TEST_RUNNER,
    LINUX_CPU_TEST_RUNNER,
}

LABEL_CIFLOW_ALL = "ciflow/all"
LABEL_CIFLOW_BAZEL = "ciflow/bazel"
LABEL_CIFLOW_CPU = "ciflow/cpu"
LABEL_CIFLOW_CUDA = "ciflow/cuda"
LABEL_CIFLOW_DEFAULT = "ciflow/default"
LABEL_CIFLOW_LIBTORCH = "ciflow/libtorch"
LABEL_CIFLOW_LINUX = "ciflow/linux"
LABEL_CIFLOW_MOBILE = "ciflow/mobile"
LABEL_CIFLOW_SANITIZERS = "ciflow/sanitizers"
LABEL_CIFLOW_ONNX = "ciflow/onnx"
LABEL_CIFLOW_SCHEDULED = "ciflow/scheduled"
LABEL_CIFLOW_SLOW = "ciflow/slow"
LABEL_CIFLOW_WIN = "ciflow/win"
LABEL_CIFLOW_XLA = "ciflow/xla"
LABEL_CIFLOW_NOARCH = "ciflow/noarch"
LABEL_CIFLOW_VULKAN = "ciflow/vulkan"
LABEL_CIFLOW_PREFIX = "ciflow/"
LABEL_CIFLOW_SLOW_GRADCHECK = "ciflow/slow-gradcheck"
LABEL_CIFLOW_DOCKER = "ciflow/docker"


@dataclass
class CIFlowConfig:
    # For use to enable workflows to run on pytorch/pytorch-canary
    run_on_canary: bool = False
    labels: Set[str] = field(default_factory=set)
    trigger_action: str = 'unassigned'
    trigger_actor: str = 'pytorchbot'
    root_job_name: str = 'ciflow_should_run'
    root_job_condition: str = ''
    label_conditions: str = ''

    def gen_root_job_condition(self) -> None:
        # CIFlow conditions:
        #  - Workflow should always run on push
        #  - CIFLOW_DEFAULT workflows should run on PRs even if no `ciflow/` labels on PR
        #  - Otherwise workflow should be scheduled on all qualifying events
        label_conditions = [f"contains(github.event.pull_request.labels.*.name, '{label}')" for label in sorted(self.labels)]
        self.label_conditions = ' || '.join(label_conditions)
        repo_condition = "github.repository_owner == 'pytorch'" if self.run_on_canary else "github.repository == 'pytorch/pytorch'"
        push_event = "github.event_name == 'push'"
        scheduled_event = "github.event_name == 'schedule'"
        pr_updated_event = f"github.event_name == 'pull_request' && github.event.action != '{self.trigger_action}'"
        if LABEL_CIFLOW_DEFAULT in self.labels:
            run_with_no_labels = f"({pr_updated_event}) && " \
                                 f"!contains(join(github.event.pull_request.labels.*.name), '{LABEL_CIFLOW_PREFIX}')"
        else:
            run_with_no_labels = "false"
        self.root_job_condition = f"${{{{ ({repo_condition}) && (\n" \
                                  f"            ({push_event}) ||\n" \
                                  f"            ({scheduled_event}) ||\n" \
                                  f"            ({self.label_conditions}) ||\n" \
                                  f"            ({run_with_no_labels}))\n"\
                                  f"         }}}}"

    def reset_root_job(self) -> None:
        self.root_job_name = ''
        self.root_job_condition = ''

    def __post_init__(self) -> None:
        self.labels.add(LABEL_CIFLOW_ALL)
        assert all(label.startswith(LABEL_CIFLOW_PREFIX) for label in self.labels)
        self.gen_root_job_condition()


@dataclass
class CIFlowRuleset:
    version = 'v1'
    output_file = f'{GITHUB_DIR}/generated-ciflow-ruleset.json'
    label_rules: Dict[str, Set[str]] = field(default_factory=dict)

    def add_label_rule(self, labels: Set[str], workflow_name: str) -> None:
        for label in labels:
            if label in self.label_rules:
                self.label_rules[label].add(workflow_name)
            else:
                self.label_rules[label] = {workflow_name}

    def generate_json(self) -> None:
        GENERATED = "generated"  # Note that please keep the variable GENERATED otherwise phabricator will hide the whole file
        output = {
            "__comment": f"@{GENERATED} DO NOT EDIT MANUALLY, Generation script: .github/scripts/generate_ci_workflows.py",
            "version": self.version,
            "label_rules": {
                label: sorted(list(workflows))
                for label, workflows in self.label_rules.items()
            }
        }
        with open(self.output_file, 'w') as outfile:
            json.dump(output, outfile, indent=2, sort_keys=True)
            outfile.write('\n')


@dataclass
class CIWorkflow:
    # Required fields
    arch: Arch
    build_environment: str
    test_runner_type: str

    # Optional fields
    ciflow_config: CIFlowConfig = field(default_factory=CIFlowConfig)
    cuda_version: str = ''
    docker_image_base: str = ''
    enable_doc_jobs: bool = False
    exclude_test: bool = False
    build_generates_artifacts: bool = True
    is_scheduled: str = ''
    num_test_shards: int = 1
    only_run_smoke_tests_on_pull_request: bool = False
    num_test_shards_on_pull_request: int = -1
    distributed_test: bool = True
    timeout_after: int = 240

    # The following variables will be set as environment variables,
    # so it's easier for both shell and Python scripts to consume it if false is represented as the empty string.
    enable_jit_legacy_test: YamlShellBool = "''"
    enable_distributed_test: YamlShellBool = "''"
    enable_multigpu_test: YamlShellBool = "''"
    enable_nogpu_no_avx_test: YamlShellBool = "''"
    enable_nogpu_no_avx2_test: YamlShellBool = "''"
    enable_slow_test: YamlShellBool = "''"
    enable_docs_test: YamlShellBool = "''"
    enable_backwards_compat_test: YamlShellBool = "''"
    enable_xla_test: YamlShellBool = "''"
    enable_noarch_test: YamlShellBool = "''"
    enable_force_on_cpu_test: YamlShellBool = "''"

    def __post_init__(self) -> None:
        if not self.build_generates_artifacts:
            self.exclude_test = True

        if self.distributed_test:
            self.enable_distributed_test = 1

        # If num_test_shards_on_pull_request is not user-defined, default to num_test_shards unless we are
        # only running smoke tests on the pull request.
        if self.num_test_shards_on_pull_request == -1:
            # Don't run the default if we are only running smoke tests
            if self.only_run_smoke_tests_on_pull_request:
                self.num_test_shards_on_pull_request = 0
            else:
                self.num_test_shards_on_pull_request = self.num_test_shards
        self.assert_valid()

    def assert_valid(self) -> None:
        err_message = f"invalid test_runner_type for {self.arch}: {self.test_runner_type}"
        if self.arch == 'linux':
            assert self.test_runner_type in LINUX_RUNNERS, err_message
        if self.arch == 'windows':
            assert self.test_runner_type in WINDOWS_RUNNERS, err_message

        assert LABEL_CIFLOW_ALL in self.ciflow_config.labels
        assert LABEL_CIFLOW_ALL in self.ciflow_config.label_conditions
        if self.arch == 'linux':
            assert LABEL_CIFLOW_LINUX in self.ciflow_config.labels
        if self.arch == 'windows':
            assert LABEL_CIFLOW_WIN in self.ciflow_config.labels
        if self.test_runner_type in CUDA_RUNNERS:
            assert LABEL_CIFLOW_CUDA in self.ciflow_config.labels
        if self.test_runner_type in CPU_RUNNERS and not self.exclude_test:
            assert LABEL_CIFLOW_CPU in self.ciflow_config.labels
        if self.is_scheduled:
            assert LABEL_CIFLOW_DEFAULT not in self.ciflow_config.labels
            assert LABEL_CIFLOW_SCHEDULED in self.ciflow_config.labels

    def generate_workflow_file(self, workflow_template: jinja2.Template) -> None:
        output_file_path = GITHUB_DIR / f"workflows/generated-{self.build_environment}.yml"
        with open(output_file_path, "w") as output_file:
            GENERATED = "generated"  # Note that please keep the variable GENERATED otherwise phabricator will hide the whole file
            output_file.writelines([f"# @{GENERATED} DO NOT EDIT MANUALLY\n"])
            try:
                content = workflow_template.render(asdict(self))
            except Exception as e:
                print(f"Failed on template: {workflow_template}", file=sys.stderr)
                raise e
            output_file.write(content)
            if content[-1] != "\n":
                output_file.write("\n")
        print(output_file_path)

@dataclass
class DockerWorkflow:
    build_environment: str
    docker_images: List[str]

    # Optional fields
    ciflow_config: CIFlowConfig = field(default_factory=CIFlowConfig)
    cuda_version: str = ''
    is_scheduled: str = ''

    def generate_workflow_file(self, workflow_template: jinja2.Template) -> None:
        output_file_path = GITHUB_DIR / "workflows/generated-docker-builds.yml"
        with open(output_file_path, "w") as output_file:
            GENERATED = "generated"  # Note that please keep the variable GENERATED otherwise phabricator will hide the whole file
            output_file.writelines([f"# @{GENERATED} DO NOT EDIT MANUALLY\n"])
            try:
                content = workflow_template.render(asdict(self))
            except Exception as e:
                print(f"Failed on template: {workflow_template}", file=sys.stderr)
                raise e
            output_file.write(content)
            if content[-1] != "\n":
                output_file.write("\n")
        print(output_file_path)

WINDOWS_WORKFLOWS = [
    CIWorkflow(
        arch="windows",
        build_environment="win-vs2019-cpu-py3",
        cuda_version="cpu",
        test_runner_type=WINDOWS_CPU_TEST_RUNNER,
        num_test_shards=2,
        ciflow_config=CIFlowConfig(
            run_on_canary=True,
            labels={LABEL_CIFLOW_DEFAULT, LABEL_CIFLOW_CPU, LABEL_CIFLOW_WIN}
        ),
    ),
    CIWorkflow(
        arch="windows",
        build_environment="win-vs2019-cuda11.3-py3",
        cuda_version="11.3",
        test_runner_type=WINDOWS_CUDA_TEST_RUNNER,
        num_test_shards=2,
        only_run_smoke_tests_on_pull_request=True,
        enable_force_on_cpu_test=1,
        ciflow_config=CIFlowConfig(
            run_on_canary=True,
            labels={LABEL_CIFLOW_DEFAULT, LABEL_CIFLOW_CUDA, LABEL_CIFLOW_WIN}
        ),
    ),
    CIWorkflow(
        arch="windows",
        build_environment="periodic-win-vs2019-cuda11.1-py3",
        cuda_version="11.1",
        test_runner_type=WINDOWS_CUDA_TEST_RUNNER,
        num_test_shards=2,
        is_scheduled="45 0,4,8,12,16,20 * * *",
        ciflow_config=CIFlowConfig(
            labels={LABEL_CIFLOW_SCHEDULED, LABEL_CIFLOW_WIN, LABEL_CIFLOW_CUDA}
        ),
    ),
]

LINUX_WORKFLOWS = [
    CIWorkflow(
        arch="linux",
        build_environment="linux-xenial-py3.6-gcc5.4",
        docker_image_base=f"{DOCKER_REGISTRY}/pytorch/pytorch-linux-xenial-py3.6-gcc5.4",
        test_runner_type=LINUX_CPU_TEST_RUNNER,
        enable_jit_legacy_test=1,
        enable_doc_jobs=True,
        enable_docs_test=1,
        enable_backwards_compat_test=1,
        num_test_shards=2,
        ciflow_config=CIFlowConfig(
            run_on_canary=True,
            labels={LABEL_CIFLOW_DEFAULT, LABEL_CIFLOW_LINUX, LABEL_CIFLOW_CPU}
        ),
    ),
    CIWorkflow(
        arch="linux",
        build_environment="linux-xenial-py3.6-gcc7",
        docker_image_base=f"{DOCKER_REGISTRY}/pytorch/pytorch-linux-xenial-py3.6-gcc7",
        test_runner_type=LINUX_CPU_TEST_RUNNER,
        num_test_shards=2,
        ciflow_config=CIFlowConfig(
            run_on_canary=True,
            labels={LABEL_CIFLOW_DEFAULT, LABEL_CIFLOW_LINUX, LABEL_CIFLOW_CPU}
        ),
    ),
    # ParallelTBB does not have a maintainer and is currently flaky
    # CIWorkflow(
    #    arch="linux",
    #    build_environment="paralleltbb-linux-xenial-py3.6-gcc5.4",
    #    docker_image_base=f"{DOCKER_REGISTRY}/pytorch/pytorch-linux-xenial-py3.6-gcc5.4",
    #    test_runner_type=LINUX_CPU_TEST_RUNNER,
    #    ciflow_config=CIFlowConfig(
    #        labels={LABEL_CIFLOW_LINUX, LABEL_CIFLOW_CPU},
    #    ),
    # ),
    CIWorkflow(
        arch="linux",
        build_environment="parallelnative-linux-xenial-py3.6-gcc5.4",
        docker_image_base=f"{DOCKER_REGISTRY}/pytorch/pytorch-linux-xenial-py3.6-gcc5.4",
        test_runner_type=LINUX_CPU_TEST_RUNNER,
        ciflow_config=CIFlowConfig(
            labels={LABEL_CIFLOW_LINUX, LABEL_CIFLOW_CPU},
        ),
    ),
    # Build PyTorch with BUILD_CAFFE2=ON
    CIWorkflow(
        arch="linux",
        build_environment="caffe2-linux-xenial-py3.6-gcc5.4",
        docker_image_base=f"{DOCKER_REGISTRY}/pytorch/pytorch-linux-xenial-py3.6-gcc5.4",
        test_runner_type=LINUX_CPU_TEST_RUNNER,
        exclude_test=True,
        ciflow_config=CIFlowConfig(
            labels={LABEL_CIFLOW_LINUX, LABEL_CIFLOW_CPU},
        ),
    ),
    CIWorkflow(
        arch="linux",
        build_environment="linux-xenial-py3-clang5-mobile-build",
        docker_image_base=f"{DOCKER_REGISTRY}/pytorch/pytorch-linux-xenial-py3-clang5-asan",
        test_runner_type=LINUX_CPU_TEST_RUNNER,
        build_generates_artifacts=False,
        exclude_test=True,
        ciflow_config=CIFlowConfig(
            labels={LABEL_CIFLOW_LINUX, LABEL_CIFLOW_MOBILE, LABEL_CIFLOW_DEFAULT},
        ),
    ),
    CIWorkflow(
        arch="linux",
        build_environment="linux-xenial-py3-clang5-mobile-custom-build-dynamic",
        docker_image_base=f"{DOCKER_REGISTRY}/pytorch/pytorch-linux-xenial-py3-clang5-android-ndk-r19c",
        test_runner_type=LINUX_CPU_TEST_RUNNER,
        build_generates_artifacts=False,
        exclude_test=True,
        ciflow_config=CIFlowConfig(
            labels={LABEL_CIFLOW_LINUX, LABEL_CIFLOW_MOBILE, LABEL_CIFLOW_DEFAULT},
        ),
    ),
    CIWorkflow(
        arch="linux",
        build_environment="linux-xenial-py3-clang5-mobile-custom-build-static",
        docker_image_base=f"{DOCKER_REGISTRY}/pytorch/pytorch-linux-xenial-py3-clang5-android-ndk-r19c",
        test_runner_type=LINUX_CPU_TEST_RUNNER,
        build_generates_artifacts=False,
        exclude_test=True,
        ciflow_config=CIFlowConfig(
            labels={LABEL_CIFLOW_LINUX, LABEL_CIFLOW_MOBILE, LABEL_CIFLOW_DEFAULT},
        ),
    ),
    CIWorkflow(
        arch="linux",
        build_environment="linux-xenial-py3-clang5-mobile-code-analysis",
        docker_image_base=f"{DOCKER_REGISTRY}/pytorch/pytorch-linux-xenial-py3-clang5-android-ndk-r19c",
        test_runner_type=LINUX_CPU_TEST_RUNNER,
        build_generates_artifacts=False,
        exclude_test=True,
        ciflow_config=CIFlowConfig(
            labels={LABEL_CIFLOW_LINUX, LABEL_CIFLOW_MOBILE},
        ),
    ),
    CIWorkflow(
        arch="linux",
        build_environment="linux-xenial-py3.6-clang7-asan",
        docker_image_base=f"{DOCKER_REGISTRY}/pytorch/pytorch-linux-xenial-py3-clang7-asan",
        test_runner_type=LINUX_CPU_TEST_RUNNER,
        num_test_shards=2,
        distributed_test=False,
        ciflow_config=CIFlowConfig(
            labels={LABEL_CIFLOW_DEFAULT, LABEL_CIFLOW_LINUX, LABEL_CIFLOW_SANITIZERS, LABEL_CIFLOW_CPU},
        ),
    ),
    CIWorkflow(
        arch="linux",
        build_environment="linux-xenial-py3.6-clang7-onnx",
        docker_image_base=f"{DOCKER_REGISTRY}/pytorch/pytorch-linux-xenial-py3-clang7-onnx",
        test_runner_type=LINUX_CPU_TEST_RUNNER,
        num_test_shards=2,
        distributed_test=False,
        ciflow_config=CIFlowConfig(
            labels={LABEL_CIFLOW_DEFAULT, LABEL_CIFLOW_LINUX, LABEL_CIFLOW_ONNX, LABEL_CIFLOW_CPU},
        ),
    ),
    CIWorkflow(
        arch="linux",
        build_environment="linux-bionic-cuda10.2-py3.9-gcc7",
        docker_image_base=f"{DOCKER_REGISTRY}/pytorch/pytorch-linux-bionic-cuda10.2-cudnn7-py3.9-gcc7",
        test_runner_type=LINUX_CUDA_TEST_RUNNER,
        enable_jit_legacy_test=1,
        enable_multigpu_test=1,
        enable_nogpu_no_avx_test=1,
        enable_nogpu_no_avx2_test=1,
        enable_slow_test=1,
        num_test_shards=2,
        ciflow_config=CIFlowConfig(
            run_on_canary=True,
            labels={LABEL_CIFLOW_SLOW, LABEL_CIFLOW_LINUX, LABEL_CIFLOW_CUDA}
        ),
    ),
    CIWorkflow(
        arch="linux",
        build_environment="libtorch-linux-xenial-cuda10.2-py3.6-gcc7",
        docker_image_base=f"{DOCKER_REGISTRY}/pytorch/pytorch-linux-xenial-cuda10.2-cudnn7-py3-gcc7",
        test_runner_type=LINUX_CUDA_TEST_RUNNER,
        build_generates_artifacts=False,
        exclude_test=True,
        ciflow_config=CIFlowConfig(
            labels=set([LABEL_CIFLOW_LIBTORCH, LABEL_CIFLOW_LINUX, LABEL_CIFLOW_CUDA]),
        ),
    ),
    CIWorkflow(
        arch="linux",
        build_environment="linux-xenial-cuda11.3-py3.6-gcc7",
        docker_image_base=f"{DOCKER_REGISTRY}/pytorch/pytorch-linux-xenial-cuda11.3-cudnn8-py3-gcc7",
        test_runner_type=LINUX_CUDA_TEST_RUNNER,
        num_test_shards=2,
        ciflow_config=CIFlowConfig(
            labels=set([LABEL_CIFLOW_DEFAULT, LABEL_CIFLOW_LINUX, LABEL_CIFLOW_CUDA]),
        ),
    ),
    CIWorkflow(
        arch="linux",
        build_environment="libtorch-linux-xenial-cuda11.3-py3.6-gcc7",
        docker_image_base=f"{DOCKER_REGISTRY}/pytorch/pytorch-linux-xenial-cuda11.3-cudnn8-py3-gcc7",
        test_runner_type=LINUX_CUDA_TEST_RUNNER,
        build_generates_artifacts=False,
        exclude_test=True,
        ciflow_config=CIFlowConfig(
            labels=set([LABEL_CIFLOW_LIBTORCH, LABEL_CIFLOW_LINUX, LABEL_CIFLOW_CUDA]),
        ),
    ),
    CIWorkflow(
        arch="linux",
        build_environment="periodic-linux-xenial-cuda11.1-py3.6-gcc7",
        docker_image_base=f"{DOCKER_REGISTRY}/pytorch/pytorch-linux-xenial-cuda11.1-cudnn8-py3-gcc7",
        test_runner_type=LINUX_CUDA_TEST_RUNNER,
        num_test_shards=2,
        is_scheduled="45 0,4,8,12,16,20 * * *",
        ciflow_config=CIFlowConfig(
            labels={LABEL_CIFLOW_SCHEDULED, LABEL_CIFLOW_LINUX, LABEL_CIFLOW_CUDA}
        ),
    ),
    CIWorkflow(
        arch="linux",
        build_environment="periodic-libtorch-linux-xenial-cuda11.1-py3.6-gcc7",
        docker_image_base=f"{DOCKER_REGISTRY}/pytorch/pytorch-linux-xenial-cuda11.1-cudnn8-py3-gcc7",
        test_runner_type=LINUX_CUDA_TEST_RUNNER,
        build_generates_artifacts=False,
        exclude_test=True,
        is_scheduled="45 0,4,8,12,16,20 * * *",
        ciflow_config=CIFlowConfig(
            labels={LABEL_CIFLOW_SCHEDULED, LABEL_CIFLOW_LINUX, LABEL_CIFLOW_LIBTORCH, LABEL_CIFLOW_CUDA},
        ),
    ),
    CIWorkflow(
        arch="linux",
        build_environment="linux-bionic-py3.6-clang9",
        docker_image_base=f"{DOCKER_REGISTRY}/pytorch/pytorch-linux-bionic-py3.6-clang9",
        test_runner_type=LINUX_CPU_TEST_RUNNER,
        num_test_shards=2,
        distributed_test=False,
        enable_noarch_test=1,
        ciflow_config=CIFlowConfig(
            labels={LABEL_CIFLOW_DEFAULT, LABEL_CIFLOW_LINUX, LABEL_CIFLOW_CPU, LABEL_CIFLOW_XLA, LABEL_CIFLOW_NOARCH},
        ),
    ),
    CIWorkflow(
        arch="linux",
        build_environment="linux-vulkan-bionic-py3.6-clang9",
        docker_image_base=f"{DOCKER_REGISTRY}/pytorch/pytorch-linux-bionic-py3.6-clang9",
        test_runner_type=LINUX_CPU_TEST_RUNNER,
        num_test_shards=1,
        distributed_test=False,
        ciflow_config=CIFlowConfig(
            labels={LABEL_CIFLOW_DEFAULT, LABEL_CIFLOW_LINUX, LABEL_CIFLOW_CPU, LABEL_CIFLOW_VULKAN},
        ),
    ),
    CIWorkflow(
        arch="linux",
        build_environment="periodic-linux-xenial-cuda10.2-py3-gcc7-slow-gradcheck",
        docker_image_base=f"{DOCKER_REGISTRY}/pytorch/pytorch-linux-xenial-cuda10.2-cudnn7-py3-gcc7",
        test_runner_type=LINUX_CUDA_TEST_RUNNER,
        num_test_shards=2,
        distributed_test=False,
        timeout_after=360,
        # Only run this on master 4 times per day since it does take a while
        is_scheduled="0 */4 * * *",
        ciflow_config=CIFlowConfig(
            labels={LABEL_CIFLOW_LINUX, LABEL_CIFLOW_CUDA, LABEL_CIFLOW_SLOW_GRADCHECK, LABEL_CIFLOW_SLOW, LABEL_CIFLOW_SCHEDULED},
        ),
    ),
]


BAZEL_WORKFLOWS = [
    CIWorkflow(
        arch="linux",
        build_environment="linux-xenial-py3.6-gcc7-bazel-test",
        docker_image_base=f"{DOCKER_REGISTRY}/pytorch/pytorch-linux-bionic-cuda10.2-cudnn7-py3.9-gcc7",
        test_runner_type=LINUX_CPU_TEST_RUNNER,
        ciflow_config=CIFlowConfig(
            labels={LABEL_CIFLOW_DEFAULT, LABEL_CIFLOW_BAZEL, LABEL_CIFLOW_CPU, LABEL_CIFLOW_LINUX},
        ),
    ),
]

DOCKER_IMAGES = {
    f"{DOCKER_REGISTRY}/pytorch/pytorch-linux-bionic-cuda10.2-cudnn7-py3.6-clang9",  # for pytorch/xla
    f"{DOCKER_REGISTRY}/pytorch/pytorch-linux-bionic-rocm4.1-py3.6",                 # for rocm
    f"{DOCKER_REGISTRY}/pytorch/pytorch-linux-bionic-rocm4.2-py3.6",                 # for rocm
    f"{DOCKER_REGISTRY}/pytorch/pytorch-linux-bionic-rocm4.3.1-py3.6",               # for rocm
}

DOCKER_IMAGES.update({
    workflow.docker_image_base
    for workflow in [*LINUX_WORKFLOWS, *BAZEL_WORKFLOWS]
    if workflow.docker_image_base
})

DOCKER_WORKFLOWS = [
    DockerWorkflow(
        build_environment="docker-builds",
        docker_images=sorted(DOCKER_IMAGES),
        # Run weekly to ensure they can build
        is_scheduled="1 * */7 * *",
    ),
]

def main() -> None:
    jinja_env = jinja2.Environment(
        variable_start_string="!{{",
        loader=jinja2.FileSystemLoader(str(GITHUB_DIR.joinpath("templates"))),
        undefined=jinja2.StrictUndefined,
    )
    template_and_workflows = [
        (jinja_env.get_template("linux_ci_workflow.yml.j2"), LINUX_WORKFLOWS),
        (jinja_env.get_template("windows_ci_workflow.yml.j2"), WINDOWS_WORKFLOWS),
        (jinja_env.get_template("bazel_ci_workflow.yml.j2"), BAZEL_WORKFLOWS),
        (jinja_env.get_template("docker_builds_ci_workflow.yml.j2"), DOCKER_WORKFLOWS),
    ]
    # Delete the existing generated files first, this should align with .gitattributes file description.
    existing_workflows = GITHUB_DIR.glob("workflows/generated-*")
    for w in existing_workflows:
        try:
            os.remove(w)
        except Exception as e:
            print(f"Error occurred when deleting file {w}: {e}")

    ciflow_ruleset = CIFlowRuleset()
    for template, workflows in template_and_workflows:
        # added Iterable check to appease the mypy gods
        if not isinstance(workflows, Iterable):
            raise Exception(f"How is workflows not iterable? {workflows}")
        for workflow in workflows:
            workflow.generate_workflow_file(workflow_template=template)
            ciflow_ruleset.add_label_rule(workflow.ciflow_config.labels, workflow.build_environment)
    ciflow_ruleset.generate_json()


if __name__ == "__main__":
    main()
