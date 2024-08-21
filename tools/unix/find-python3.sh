
get_python3() {
  # Search for python3
  python=$(which python3 2>/dev/null)

  # If not found, search for python
  if [ -z "$python" ]; then
    python=$(which python 2>/dev/null)
    if [ -n "$python" ]; then
      local python_version=$($python -V 2>&1 | head -n 1 | cut -d ' ' -f 2- | cut -d '.' -f1)
      if [ "$python_version" != "3" ]; then
        unset python
      fi
    fi
  fi

  if [ -z "$python" ]; then
    return
  fi
  echo "$python"
}
