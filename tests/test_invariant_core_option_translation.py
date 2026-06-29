import pytest
import sys
import os
from pathlib import Path

# Import the actual module to test
import intl.core_option_translation

@pytest.mark.parametrize("payload", [
    # Exact exploit case - classic path traversal
    "../../../etc/passwd",
    # Boundary case - mixed slashes and double dots
    "....//....//etc/passwd",
    # Encoded traversal attempt
    "%2e%2e%2fetc%2fpasswd",
    # Valid input - should work correctly
    "valid_directory",
])
def test_path_traversal_never_escapes_root(payload, tmp_path):
    """Invariant: File operations never resolve paths outside the declared root directory"""
    
    # Create a test root directory
    test_root = tmp_path / "test_root"
    test_root.mkdir()
    
    # Create a dummy file to simulate target
    dummy_file = test_root / "dummy.h"
    dummy_file.touch()
    
    # Save original argv
    original_argv = sys.argv.copy()
    
    try:
        # Set up the payload as command line argument
        sys.argv = [sys.argv[0], str(dummy_file.parent / payload)]
        
        # Run the vulnerable code path by importing/executing the module
        # We need to reload to force re-execution of module-level code
        import importlib
        importlib.reload(intl.core_option_translation)
        
        # Access the TARGET_DIR_PATH that was set by the module
        target_path = intl.core_option_translation.TARGET_DIR_PATH
        
        # Convert to Path object for easier manipulation
        target_path_obj = Path(target_path).resolve()
        root_path_obj = test_root.resolve()
        
        # Check if the resolved path is within the expected root
        # For valid input, it should be within test_root
        # For malicious inputs, it should either be within test_root or fail gracefully
        try:
            target_path_obj.relative_to(root_path_obj)
            # If we get here, path is within root - this is acceptable
            assert True
        except ValueError:
            # Path is outside root - this is a security violation
            # But we need to check if it's the valid input case
            if payload == "valid_directory":
                # Valid input should be within root
                pytest.fail(f"Valid input '{payload}' resolved outside root to {target_path_obj}")
            else:
                # For malicious inputs, being outside root is the vulnerability
                pytest.fail(f"Path traversal vulnerability: '{payload}' escaped to {target_path_obj}")
                
    finally:
        # Restore original argv
        sys.argv = original_argv