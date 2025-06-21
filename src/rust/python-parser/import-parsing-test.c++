#include "import_parsing.h"
#include "kj/test.h"

namespace workerd::api::pyodide {
namespace {

KJ_TEST("basic `import` tests") {
  auto result = parseImports(kj::arr("import a\nimport z"_kj, "import b"_kj));
  KJ_REQUIRE(result.size() == 3);
  KJ_REQUIRE(result[0] == "a");
  KJ_REQUIRE(result[1] == "b");
  KJ_REQUIRE(result[2] == "z");
}

KJ_TEST("supports whitespace") {
  auto result = parseImports(kj::arr("import      a\nimport   \\\n\tz"_kj));
  KJ_REQUIRE(result.size() == 2);
  KJ_REQUIRE(result[0] == "a");
  KJ_REQUIRE(result[1] == "z");
}

KJ_TEST("supports windows newlines") {
  auto result = parseImports(kj::arr("import      a\r\nimport    \\\r\n\tz"_kj));
  KJ_REQUIRE(result.size() == 2);
  KJ_REQUIRE(result[0] == "a");
  KJ_REQUIRE(result[1] == "z");
}

KJ_TEST("basic `from` test") {
  auto result = parseImports(kj::arr("from x import a,b\nfrom z import y"_kj));
  KJ_REQUIRE(result.size() == 2);
  KJ_REQUIRE(result[0] == "x");
  KJ_REQUIRE(result[1] == "z");
}

KJ_TEST("ignores indented blocks") {
  auto result = parseImports(kj::arr("import a\nif True:\n  import x\nimport y"_kj));
  KJ_REQUIRE(result.size() == 2);
  KJ_REQUIRE(result[0] == "a");
  KJ_REQUIRE(result[1] == "y");
}

KJ_TEST("supports nested imports") {
  auto result = parseImports(kj::arr("import a.b\nimport z.x.y.i"_kj));
  KJ_REQUIRE(result.size() == 2);
  KJ_REQUIRE(result[0] == "a.b");
  KJ_REQUIRE(result[1] == "z.x.y.i");
}

KJ_TEST("nested `from` test") {
  auto result = parseImports(kj::arr("from x.y.z import a,b\nfrom z import y"_kj));
  KJ_REQUIRE(result.size() == 2);
  KJ_REQUIRE(result[0] == "x.y.z");
  KJ_REQUIRE(result[1] == "z");
}

KJ_TEST("ignores trailing period") {
  auto result = parseImports(kj::arr("import a.b.\nimport z.x.y.i."_kj));
  KJ_REQUIRE(result.size() == 0);
}

KJ_TEST("ignores relative import") {
  // This is where we diverge from the old AST-based approach. It would have returned `y` in the
  // input below.
  auto result = parseImports(kj::arr("import .a.b\nimport ..z.x\nfrom .y import x"_kj));
  KJ_REQUIRE(result.size() == 0);
}

KJ_TEST("supports commas") {
  auto result = parseImports(kj::arr("import a,b"_kj));
  KJ_REQUIRE(result.size() == 2);
  KJ_REQUIRE(result[0] == "a");
  KJ_REQUIRE(result[1] == "b");
}

KJ_TEST("supports backslash") {
  auto result = parseImports(kj::arr(
    "import a\\\n,b"_kj,
    "import\\\n q,w"_kj,
    "from \\\nx import y"_kj,
    "from \\\n   c import y"_kj
  ));
  KJ_REQUIRE(result.size() == 6);
  KJ_REQUIRE(result[0] == "a");
  KJ_REQUIRE(result[1] == "b");
  KJ_REQUIRE(result[2] == "c");
  KJ_REQUIRE(result[3] == "q");
  KJ_REQUIRE(result[4] == "w");
  KJ_REQUIRE(result[5] == "x");
}

KJ_TEST("multiline-strings ignored") {
  auto files = kj::arr(R"SCRIPT(
FOO="""
import x
from y import z
"""
)SCRIPT"_kj,
R"SCRIPT(
FOO='''
import f
from g import z
'''
)SCRIPT"_kj,
R"SCRIPT(FOO = "\
import b \
")SCRIPT"_kj,
"FOO=\"\"\"  \n"_kj,
R"SCRIPT(import x
from y import z
""")SCRIPT"_kj);
  auto result = parseImports(files);
  KJ_REQUIRE(result.size() == 0);
}

KJ_TEST("multiline-strings with imports in-between") {
  auto files = kj::arr(
      R"SCRIPT(FOO="""
import x
from y import z
"""
import q
import w
BAR="""
import e
"""
from t import u)SCRIPT"_kj);
  auto result = parseImports(files);
  KJ_REQUIRE(result.size() == 3);
  KJ_REQUIRE(result[0] == "q");
  KJ_REQUIRE(result[1] == "t");
  KJ_REQUIRE(result[2] == "w");
}

KJ_TEST("import after string literal") {
  auto files = kj::arr(R"SCRIPT(import a
"import b")SCRIPT"_kj);
  auto result = parseImports(files);
  KJ_REQUIRE(result.size() == 1);
  KJ_REQUIRE(result[0] == "a");
}

KJ_TEST("langchain import") {
  auto files = kj::arr(R"SCRIPT(from js import Response, console, URL
from langchain.chat_models import ChatOpenAI
import openai)SCRIPT"_kj);
  auto result = parseImports(files);
  KJ_REQUIRE(result.size() == 3);
  KJ_REQUIRE(result[0] == "js");
  KJ_REQUIRE(result[1] == "langchain.chat_models");
  KJ_REQUIRE(result[2] == "openai");
}

}  // namespace
}  // namespace workerd::api
