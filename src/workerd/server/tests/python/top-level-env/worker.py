from js import Reflect
from workers import WorkerEntrypoint, env, patch_env


def check_normal():
    assert env.secret == "thisisasecret"
    assert not hasattr(env, "NAME")
    assert not hasattr(env, "place")
    assert not hasattr(env, "extra")
    assert Reflect.ownKeys(env).to_py() == ["secret"]


class Default(WorkerEntrypoint):
    def test(self):
        check_normal()

        with patch_env(NAME=1, place="Somewhere"):
            assert env.NAME == 1
            assert env.place == "Somewhere"
            assert not hasattr(env, "secret")

        check_normal()

        with patch_env({"NAME": 1, "place": "Somewhere"}):
            assert env.NAME == 1
            assert env.place == "Somewhere"
            assert not hasattr(env, "secret")

        check_normal()

        with patch_env([("NAME", 1), ("place", "Somewhere")]):
            assert env.NAME == 1
            assert env.place == "Somewhere"
            assert not hasattr(env, "secret")

        check_normal()

        with patch_env({"NAME": 1, "place": "Somewhere"}, extra=22):
            assert env.NAME == 1
            assert env.place == "Somewhere"
            assert env.extra == 22
            assert not hasattr(env, "secret")

        check_normal()

        try:
            with patch_env(NAME=1, place="Somewhere"):
                raise RuntimeError("oops")  # noqa: TRY301
        except Exception:
            pass

        check_normal()
