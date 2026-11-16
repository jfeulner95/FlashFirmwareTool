import os
import sys

# The GUI modules live next to this folder (python/) and are not an installed package.
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
