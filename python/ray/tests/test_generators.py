import pytest
import numpy as np
import sys
import time
import gc
from unittest.mock import Mock

import ray
from ray.util.client.ray_client_helpers import (
    ray_start_client_server_for_address,
)
from ray._private.client_mode_hook import enable_client_mode
from ray.tests.conftest import call_ray_start_context
from ray._common.test_utils import (
    wait_for_condition,
)


def assert_no_leak():
    def check():
        gc.collect()
        core_worker = ray._private.worker.global_worker.core_worker
        ref_counts = core_worker.get_all_reference_counts()
        for k, rc in ref_counts.items():
            if rc["local"] != 0:
                return False
            if rc["submitted"] != 0:
                return False
        return True

    wait_for_condition(check)


@pytest.mark.skipif(
    sys.platform != "linux" and sys.platform != "linux2",
    reason="This test requires Linux.",
)
def test_generator_oom(ray_start_regular_shared):
    num_returns = 100

    @ray.remote(max_retries=0)
    def large_values(num_returns):
        return [
            np.random.randint(
                np.iinfo(np.int8).max, size=(100_000_000, 1), dtype=np.int8
            )
            for _ in range(num_returns)
        ]

    @ray.remote(max_retries=0)
    def large_values_generator(num_returns):
        for _ in range(num_returns):
            yield np.random.randint(
                np.iinfo(np.int8).max, size=(100_000_000, 1), dtype=np.int8
            )

    try:
        # Worker may OOM using normal returns.
        ray.get(large_values.options(num_returns=num_returns).remote(num_returns)[0])
    except ray.exceptions.WorkerCrashedError:
        pass

    # Using a generator will allow the worker to finish.
    ray.get(
        large_values_generator.options(num_returns=num_returns).remote(num_returns)[0]
    )


@pytest.mark.parametrize("use_actors", [False, True])
@pytest.mark.parametrize("store_in_plasma", [False, True])
def test_generator_returns(ray_start_regular_shared, use_actors, store_in_plasma):
    remote_generator_fn = None
    if use_actors:

        @ray.remote
        class Generator:
            def __init__(self):
                pass

            def generator(self, num_returns, store_in_plasma):
                for i in range(num_returns):
                    if store_in_plasma:
                        yield np.ones(1_000_000, dtype=np.int8) * i
                    else:
                        yield [i]

        g = Generator.remote()
        remote_generator_fn = g.generator
    else:

        @ray.remote(max_retries=0)
        def generator(num_returns, store_in_plasma):
            for i in range(num_returns):
                if store_in_plasma:
                    yield np.ones(1_000_000, dtype=np.int8) * i
                else:
                    yield [i]

        remote_generator_fn = generator

    # Check cases when num_returns does not match the number of values returned
    # by the generator.
    num_returns = 3

    try:
        ray.get(
            remote_generator_fn.options(num_returns=num_returns).remote(
                num_returns - 1, store_in_plasma
            )
        )
        assert False
    except ray.exceptions.RayTaskError as e:
        assert isinstance(e.as_instanceof_cause(), ValueError)

    # TODO(swang): When generators return more values than expected, we log an
    # error but the exception is not thrown to the application.
    # https://github.com/ray-project/ray/issues/28689.
    ray.get(
        remote_generator_fn.options(num_returns=num_returns).remote(
            num_returns + 1, store_in_plasma
        )
    )

    # Check return values.
    [
        x[0]
        for x in ray.get(
            remote_generator_fn.options(num_returns=num_returns).remote(
                num_returns, store_in_plasma
            )
        )
    ] == list(range(num_returns))
    # Works for num_returns=1 if generator returns a single value.
    assert (
        ray.get(remote_generator_fn.options(num_returns=1).remote(1, store_in_plasma))[
            0
        ]
        == 0
    )


@pytest.mark.parametrize("use_actors", [False, True])
@pytest.mark.parametrize("store_in_plasma", [False, True])
@pytest.mark.parametrize("num_returns_type", ["dynamic", None])
def test_generator_errors(
    ray_start_regular_shared, use_actors, store_in_plasma, num_returns_type
):
    remote_generator_fn = None
    if use_actors:

        @ray.remote
        class Generator:
            def __init__(self):
                pass

            def generator(self, num_returns, store_in_plasma):
                for i in range(num_returns - 2):
                    if store_in_plasma:
                        yield np.ones(1_000_000, dtype=np.int8) * i
                    else:
                        yield [i]
                raise Exception("error")

        g = Generator.remote()
        remote_generator_fn = g.generator
    else:

        @ray.remote(max_retries=0)
        def generator(num_returns, store_in_plasma):
            for i in range(num_returns - 2):
                if store_in_plasma:
                    yield np.ones(1_000_000, dtype=np.int8) * i
                else:
                    yield [i]
            raise Exception("error")

        remote_generator_fn = generator

    ref1, ref2, ref3 = remote_generator_fn.options(num_returns=3).remote(
        3, store_in_plasma
    )
    ray.get(ref1)
    with pytest.raises(ray.exceptions.RayTaskError):
        ray.get(ref2)
    with pytest.raises(ray.exceptions.RayTaskError):
        ray.get(ref3)

    dynamic_ref = remote_generator_fn.options(num_returns=num_returns_type).remote(
        3, store_in_plasma
    )
    ref1, ref2 = ray.get(dynamic_ref)
    ray.get(ref1)
    with pytest.raises(ray.exceptions.RayTaskError):
        ray.get(ref2)


@pytest.mark.parametrize("store_in_plasma", [False, True])
@pytest.mark.parametrize("num_returns_type", ["dynamic", None])
def test_dynamic_generator_retry_exception(
    ray_start_regular_shared, store_in_plasma, num_returns_type
):
    class CustomException(Exception):
        pass

    @ray.remote(num_cpus=0)
    class ExecutionCounter:
        def __init__(self):
            self.count = 0

        def inc(self):
            self.count += 1
            return self.count

        def get_count(self):
            return self.count

        def reset(self):
            self.count = 0

    @ray.remote(max_retries=1)
    def generator(num_returns, store_in_plasma, counter):
        for i in range(num_returns):
            if store_in_plasma:
                yield np.ones(1_000_000, dtype=np.int8) * i
            else:
                yield [i]

            # Fail on first execution, succeed on next.
            if ray.get(counter.inc.remote()) == 1:
                raise CustomException("error")

    counter = ExecutionCounter.remote()
    dynamic_ref = generator.options(num_returns=num_returns_type).remote(
        3, store_in_plasma, counter
    )
    ref1, ref2 = ray.get(dynamic_ref)
    ray.get(ref1)
    with pytest.raises(ray.exceptions.RayTaskError):
        ray.get(ref2)

    ray.get(counter.reset.remote())
    dynamic_ref = generator.options(
        num_returns=num_returns_type, retry_exceptions=[CustomException]
    ).remote(3, store_in_plasma, counter)
    for i, ref in enumerate(ray.get(dynamic_ref)):
        assert ray.get(ref)[0] == i


@pytest.mark.parametrize("use_actors", [False, True])
@pytest.mark.parametrize("store_in_plasma", [False, True])
@pytest.mark.parametrize("num_returns_type", ["dynamic", None])
def test_dynamic_generator(
    ray_start_regular_shared, use_actors, store_in_plasma, num_returns_type
):
    if not use_actors:

        @ray.remote(num_returns=num_returns_type)
        def dynamic_generator(num_returns, store_in_plasma):
            for i in range(num_returns):
                if store_in_plasma:
                    yield np.ones(1_000_000, dtype=np.int8) * i
                else:
                    yield [i]

        remote_generator_fn = dynamic_generator
    else:

        @ray.remote
        class Generator:
            def __init__(self):
                pass

            def generator(self, num_returns, store_in_plasma):
                for i in range(num_returns):
                    if store_in_plasma:
                        yield np.ones(1_000_000, dtype=np.int8) * i
                    else:
                        yield [i]

        g = Generator.remote()
        remote_generator_fn = g.generator

    @ray.remote
    def read(gen):
        for i, ref in enumerate(gen):
            if ray.get(ref)[0] != i:
                return False
        return True

    gen = ray.get(
        remote_generator_fn.options(num_returns=num_returns_type).remote(
            10, store_in_plasma
        )
    )
    for i, ref in enumerate(gen):
        assert ray.get(ref)[0] == i

    # Test empty generator.
    gen = ray.get(
        remote_generator_fn.options(num_returns=num_returns_type).remote(
            0, store_in_plasma
        )
    )
    assert len(list(gen)) == 0

    # Check that passing as task arg.
    if num_returns_type == "dynamic":
        gen = remote_generator_fn.options(num_returns=num_returns_type).remote(
            10, store_in_plasma
        )
        assert ray.get(read.remote(gen))
        assert ray.get(read.remote(ray.get(gen)))
    else:
        with pytest.raises(TypeError):
            gen = remote_generator_fn.options(num_returns=num_returns_type).remote(
                10, store_in_plasma
            )
            assert ray.get(read.remote(gen))

    # Also works if we override num_returns with a static value.
    ray.get(
        read.remote(
            remote_generator_fn.options(num_returns=10).remote(10, store_in_plasma)
        )
    )

    if num_returns_type == "dynamic":
        # Normal remote functions don't work with num_returns="dynamic".
        @ray.remote(num_returns=num_returns_type)
        def static(num_returns):
            return list(range(num_returns))

        with pytest.raises(ray.exceptions.RayTaskError):
            gen = ray.get(static.remote(3))
            for ref in gen:
                ray.get(ref)


def test_dynamic_generator_gc_each_yield(ray_start_cluster):
    # Need to shutdown when going from ray_start_regular_shared to ray_start_cluster
    ray.shutdown()

    num_returns = 5

    @ray.remote(num_returns="dynamic")
    def generator():
        for i in range(num_returns):
            yield np.ones((1000, 1000), dtype=np.uint8)

    def check_ref_counts(expected):
        ref_counts = (
            ray._private.worker.global_worker.core_worker.get_all_reference_counts()
        )
        return len(ref_counts) == expected

    dynamic_ref = ray.get(generator.remote())

    for i, ref in enumerate(dynamic_ref):
        gc.collect()
        # assert references are released after each yield
        wait_for_condition(lambda: check_ref_counts(num_returns - i))
        ray.get(ref)


@pytest.mark.parametrize("num_returns_type", ["dynamic", None])
def test_dynamic_generator_distributed(ray_start_cluster, num_returns_type):
    cluster = ray_start_cluster
    # Head node with no resources.
    cluster.add_node(num_cpus=0)
    ray.init(address=cluster.address)
    cluster.add_node(num_cpus=1)
    cluster.wait_for_nodes()

    @ray.remote(num_returns=num_returns_type)
    def dynamic_generator(num_returns):
        for i in range(num_returns):
            yield np.ones(1_000_000, dtype=np.int8) * i
            time.sleep(0.1)

    gen = ray.get(dynamic_generator.remote(3))
    for i, ref in enumerate(gen):
        # Check that we can fetch the values from a different node.
        assert ray.get(ref)[0] == i


@pytest.mark.parametrize("num_returns_type", ["dynamic", None])
def test_dynamic_generator_reconstruction(ray_start_cluster, num_returns_type):
    config = {
        "health_check_failure_threshold": 10,
        "health_check_period_ms": 100,
        "health_check_initial_delay_ms": 0,
        "max_direct_call_object_size": 100,
        "task_retry_delay_ms": 100,
        "object_timeout_milliseconds": 200,
        "fetch_warn_timeout_milliseconds": 1000,
        "local_gc_min_interval_s": 1,
    }
    cluster = ray_start_cluster
    # Head node with no resources.
    cluster.add_node(
        num_cpus=0, _system_config=config, enable_object_reconstruction=True
    )
    ray.init(address=cluster.address)
    # Node to place the initial object.
    node_to_kill = cluster.add_node(num_cpus=1, object_store_memory=10**8)
    cluster.wait_for_nodes()

    @ray.remote(num_returns=num_returns_type)
    def dynamic_generator(num_returns):
        for i in range(num_returns):
            # Random ray.put to make sure it's okay to interleave these with
            # the dynamic returns.
            if np.random.randint(2) == 1:
                ray.put(np.ones(1_000_000, dtype=np.int8) * np.random.randint(100))
            yield np.ones(1_000_000, dtype=np.int8) * i

    @ray.remote
    def fetch(x):
        return x[0]

    # Test recovery of all dynamic objects through re-execution.
    gen = ray.get(dynamic_generator.remote(10))
    cluster.remove_node(node_to_kill, allow_graceful=False)
    node_to_kill = cluster.add_node(num_cpus=1, object_store_memory=10**8)
    refs = list(gen)

    for i, ref in enumerate(refs):
        print("fetching ", i)
        assert ray.get(fetch.remote(ref)) == i

    cluster.add_node(num_cpus=1, resources={"node2": 1}, object_store_memory=10**8)

    # Fetch one of the ObjectRefs to another node. We should try to reuse this
    # copy during recovery.
    ray.get(fetch.options(resources={"node2": 1}).remote(refs[-1]))
    cluster.remove_node(node_to_kill, allow_graceful=False)
    for i, ref in enumerate(refs):
        assert ray.get(fetch.remote(ref)) == i
        del ref

    del refs
    del gen
    assert_no_leak()


@pytest.mark.parametrize("too_many_returns", [False, True])
@pytest.mark.parametrize("num_returns_type", ["dynamic", None])
def test_dynamic_generator_reconstruction_nondeterministic(
    ray_start_cluster, too_many_returns, num_returns_type
):
    config = {
        "health_check_failure_threshold": 10,
        "health_check_period_ms": 100,
        "health_check_initial_delay_ms": 0,
        "max_direct_call_object_size": 100,
        "task_retry_delay_ms": 100,
        "object_timeout_milliseconds": 200,
        "fetch_warn_timeout_milliseconds": 1000,
        "local_gc_min_interval_s": 1,
    }
    cluster = ray_start_cluster
    # Head node with no resources.
    cluster.add_node(
        num_cpus=1,
        _system_config=config,
        enable_object_reconstruction=True,
        resources={"head": 1},
    )
    ray.init(address=cluster.address)
    # Node to place the initial object.
    node_to_kill = cluster.add_node(num_cpus=1, object_store_memory=10**8)
    cluster.wait_for_nodes()

    @ray.remote(num_cpus=1, resources={"head": 1})
    class FailureSignal:
        def __init__(self):
            return

        def ping(self):
            return

    @ray.remote(num_returns=num_returns_type)
    def dynamic_generator(failure_signal):
        num_returns = 10
        try:
            ray.get(failure_signal.ping.remote())
        except ray.exceptions.RayActorError:
            if too_many_returns:
                num_returns += 1
            else:
                num_returns -= 1
        for i in range(num_returns):
            yield np.ones(1_000_000, dtype=np.int8) * i

    @ray.remote
    def fetch(x):
        return

    failure_signal = FailureSignal.remote()
    gen = ray.get(dynamic_generator.remote(failure_signal))
    cluster.remove_node(node_to_kill, allow_graceful=False)
    ray.kill(failure_signal)
    refs = list(gen)
    if too_many_returns:
        for i, ref in enumerate(refs):
            assert np.array_equal(np.ones(1_000_000, dtype=np.int8) * i, ray.get(ref))
            del ref
    else:
        if num_returns_type == "dynamic":
            # If dynamic is specified, when the num_returns
            # is different, all previous refs are failed.
            with pytest.raises(ray.exceptions.RayTaskError):
                for ref in refs:
                    ray.get(ref)
                    del ref
        else:
            # Otherwise, we can reconstruct the refs again.
            # We allow it because the refs could have already obtained
            # by the generator.
            for i, ref in enumerate(refs):
                assert np.array_equal(
                    np.ones(1_000_000, dtype=np.int8) * i, ray.get(ref)
                )
                del ref
    # TODO(swang): If the re-executed task returns a different number of
    # objects, we should throw an error for every return value.
    # for ref in refs:
    #     with pytest.raises(ray.exceptions.RayTaskError):
    #         ray.get(ref)
    del gen
    del refs
    if num_returns_type is None:
        # TODO(sang): For some reasons, it fails when "dynamic"
        # is used. We don't fix the issue because we will
        # remove this flag soon anyway.
        assert_no_leak()


@pytest.mark.parametrize("num_returns_type", ["dynamic", None])
def test_dynamic_generator_reconstruction_fails(ray_start_cluster, num_returns_type):
    config = {
        "health_check_failure_threshold": 10,
        "health_check_period_ms": 100,
        "health_check_initial_delay_ms": 0,
        "max_direct_call_object_size": 100,
        "task_retry_delay_ms": 100,
        "object_timeout_milliseconds": 200,
        "fetch_warn_timeout_milliseconds": 1000,
        "local_gc_min_interval_s": 1,
    }
    cluster = ray_start_cluster
    cluster.add_node(
        num_cpus=1,
        _system_config=config,
        enable_object_reconstruction=True,
        resources={"head": 1},
    )
    ray.init(address=cluster.address)
    # Node to place the initial object.
    node_to_kill = cluster.add_node(num_cpus=1, object_store_memory=10**8)
    cluster.wait_for_nodes()

    @ray.remote(num_cpus=1, resources={"head": 1})
    class FailureSignal:
        def __init__(self):
            return

        def ping(self):
            return

    @ray.remote(num_returns=num_returns_type)
    def dynamic_generator(failure_signal):
        num_returns = 10
        for i in range(num_returns):
            yield np.ones(1_000_000, dtype=np.int8) * i
            if i == num_returns // 2:
                # If this is the re-execution, fail the worker after partial yield.
                try:
                    ray.get(failure_signal.ping.remote())
                except ray.exceptions.RayActorError:
                    sys.exit(-1)

    @ray.remote
    def fetch(*refs):
        pass

    failure_signal = FailureSignal.remote()
    gen = ray.get(dynamic_generator.remote(failure_signal))
    refs = list(gen)
    ray.get(fetch.remote(*refs))
    cluster.remove_node(node_to_kill, allow_graceful=False)
    done = fetch.remote(*refs)
    ray.kill(failure_signal)
    # Make sure we can get the error.
    with pytest.raises(ray.exceptions.WorkerCrashedError):
        for ref in refs:
            ray.get(ref)

    # Make sure other tasks can also get the error.
    with pytest.raises(ray.exceptions.RayTaskError):
        ray.get(done)

    del ref, gen, refs, done, failure_signal
    gc.collect()
    assert_no_leak()


@pytest.mark.parametrize("num_returns_type", ["dynamic", None])
def test_dynamic_empty_generator_reconstruction_nondeterministic(
    ray_start_cluster, num_returns_type
):
    config = {
        "health_check_failure_threshold": 10,
        "health_check_period_ms": 100,
        "health_check_initial_delay_ms": 0,
        "max_direct_call_object_size": 100,
        "task_retry_delay_ms": 100,
        "object_timeout_milliseconds": 200,
        "fetch_warn_timeout_milliseconds": 1000,
        "local_gc_min_interval_s": 1,
    }
    cluster = ray_start_cluster
    # Head node with no resources.
    cluster.add_node(
        num_cpus=0,
        _system_config=config,
        enable_object_reconstruction=True,
        resources={"head": 1},
    )
    ray.init(address=cluster.address)
    # Node to place the initial object.
    node_to_kill = cluster.add_node(num_cpus=1, object_store_memory=10**8)
    cluster.wait_for_nodes()

    @ray.remote(num_cpus=0, resources={"head": 1})
    class ExecutionCounter:
        def __init__(self):
            self.count = 0

        def inc(self):
            self.count += 1
            return self.count

        def get_count(self):
            return self.count

    @ray.remote(num_returns=num_returns_type)
    def maybe_empty_generator(exec_counter):
        if ray.get(exec_counter.inc.remote()) > 1:
            for i in range(3):
                yield np.ones(1_000_000, dtype=np.int8) * i

    @ray.remote
    def check(empty_generator):
        return len(list(empty_generator)) == 0

    exec_counter = ExecutionCounter.remote()
    gen = maybe_empty_generator.remote(exec_counter)
    gen = ray.get(gen)
    refs = list(gen)
    assert ray.get(check.remote(refs))
    cluster.remove_node(node_to_kill, allow_graceful=False)
    node_to_kill = cluster.add_node(num_cpus=1, object_store_memory=10**8)
    assert ray.get(check.remote(refs))

    # We should never reconstruct an empty generator.
    assert ray.get(exec_counter.get_count.remote()) == 1

    del gen, refs, exec_counter
    assert_no_leak()


def test_yield_exception(ray_start_cluster):
    @ray.remote
    def f():
        yield 1
        yield 2
        yield Exception("value")
        yield 3
        raise Exception("raise")
        yield 5

    gen = f.remote()
    assert ray.get(next(gen)) == 1
    assert ray.get(next(gen)) == 2
    yield_exc = ray.get(next(gen))
    assert isinstance(yield_exc, Exception)
    assert str(yield_exc) == "value"
    assert ray.get(next(gen)) == 3
    with pytest.raises(Exception, match="raise"):
        ray.get(next(gen))
    with pytest.raises(StopIteration):
        ray.get(next(gen))


def test_actor_yield_exception(ray_start_cluster):
    @ray.remote
    class A:
        def f(self):
            yield 1
            yield 2
            yield Exception("value")
            yield 3
            raise Exception("raise")
            yield 5

    a = A.remote()
    gen = a.f.remote()
    assert ray.get(next(gen)) == 1
    assert ray.get(next(gen)) == 2
    yield_exc = ray.get(next(gen))
    assert isinstance(yield_exc, Exception)
    assert str(yield_exc) == "value"
    assert ray.get(next(gen)) == 3
    with pytest.raises(Exception, match="raise"):
        ray.get(next(gen))
    with pytest.raises(StopIteration):
        ray.get(next(gen))


def test_async_actor_yield_exception(ray_start_cluster):
    @ray.remote
    class A:
        async def f(self):
            yield 1
            yield 2
            yield Exception("value")
            yield 3
            raise Exception("raise")
            yield 5

    a = A.remote()
    gen = a.f.remote()
    assert ray.get(next(gen)) == 1
    assert ray.get(next(gen)) == 2
    yield_exc = ray.get(next(gen))
    assert isinstance(yield_exc, Exception)
    assert str(yield_exc) == "value"
    assert ray.get(next(gen)) == 3
    with pytest.raises(Exception, match="raise"):
        ray.get(next(gen))
    with pytest.raises(StopIteration):
        ray.get(next(gen))


# Client server port of the shared Ray instance
SHARED_CLIENT_SERVER_PORT = 25555


@pytest.fixture(scope="module")
def call_ray_start_shared(request):
    request = Mock()
    request.param = (
        "ray start --head --min-worker-port=0 --max-worker-port=0 --port 0 "
        f"--ray-client-server-port={SHARED_CLIENT_SERVER_PORT}"
    )
    with call_ray_start_context(request) as address:
        yield address


@pytest.mark.parametrize("store_in_plasma", [False, True])
def test_ray_client(call_ray_start_shared, store_in_plasma):
    with ray_start_client_server_for_address(call_ray_start_shared):
        enable_client_mode()

        @ray.remote(max_retries=0)
        def generator(num_returns, store_in_plasma):
            for i in range(num_returns):
                if store_in_plasma:
                    yield np.ones(1_000_000, dtype=np.int8) * i
                else:
                    yield [i]

        # TODO(swang): When generators return more values than expected, we log an
        # error but the exception is not thrown to the application.
        # https://github.com/ray-project/ray/issues/28689.
        num_returns = 3
        ray.get(
            generator.options(num_returns=num_returns).remote(
                num_returns + 1, store_in_plasma
            )
        )

        # Check return values.
        [
            x[0]
            for x in ray.get(
                generator.options(num_returns=num_returns).remote(
                    num_returns, store_in_plasma
                )
            )
        ] == list(range(num_returns))
        # Works for num_returns=1 if generator returns a single value.
        assert (
            ray.get(generator.options(num_returns=1).remote(1, store_in_plasma))[0] == 0
        )

        gen = ray.get(
            generator.options(num_returns="dynamic").remote(3, store_in_plasma)
        )
        for i, ref in enumerate(gen):
            assert ray.get(ref)[0] == i


if __name__ == "__main__":

    sys.exit(pytest.main(["-sv", __file__]))
