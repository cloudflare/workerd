load("//src/workerd/server/tests/python:py_wd_test.bzl", "py_wd_test")

def vendored_py_wd_test(name, test_template, main_py_file, vendored_srcs_target):
    """Creates a vendored Python test with dynamic module generation.

    Args:
        name: Name of the test
        test_template: The .wd-test template file
        main_py_file: The main Python file for the test
        vendored_srcs_target: The Bazel target containing the vendored sources
    """

    # Generate module list
    module_list_name = name + "_modules_string"
    native.genrule(
        name = module_list_name,
        srcs = [
            vendored_srcs_target,
            "generate_modules.py",
        ],
        outs = [module_list_name + ".txt"],
        cmd = "python3 $(location generate_modules.py) $(locations " + vendored_srcs_target + ")  > $@",
    )

    # Perform substitution to include the generated modules in template
    substitution_name = name + "_perform_substitution"
    native.genrule(
        name = substitution_name,
        srcs = [
            test_template,
            ":" + module_list_name,
        ],
        outs = [name + ".test.generated"],
        cmd = """
        python3 -c "
import sys
with open('$(location :""" + module_list_name + """)', 'r') as f:
    modules = f.read()
with open('$(location """ + test_template + """)', 'r') as f:
    template = f.read()
result = template.replace('%PYTHON_VENDORED_MODULES%', modules)

with open('$@', 'w') as f:
    f.write(result)
        "
    """,
    )

    # Create the py_wd_test
    py_wd_test(
        name = name,
        src = ":" + substitution_name,
        data = [
            main_py_file,
            vendored_srcs_target,
        ],
    )
