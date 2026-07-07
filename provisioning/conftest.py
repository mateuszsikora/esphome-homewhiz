import os
import sys

# Make the provisioning package root importable (generate_mapping, vendor.*)
sys.path.insert(0, os.path.dirname(__file__))
