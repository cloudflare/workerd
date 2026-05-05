"""
Workflow-specific classes and exceptions for the workers module.
"""


class NonRetryableError(Exception):
    """
    A marker exception used to signal that a workflow step should not be retried.
    This is a special exception used by workflows.
    """

    pass
