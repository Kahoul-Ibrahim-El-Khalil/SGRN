#!/usr/bin/env python3
import json
import os
import sys

def split_compile_commands(master_path, project_root):
    if not os.path.exists(master_path):
        print(f"Error: Master compile_commands.json not found at {master_path}")
        return

    print(f"Reading master compile_commands.json from {master_path}...")
    with open(master_path, 'r') as f:
        try:
            data = json.load(f)
        except json.JSONDecodeError as e:
            print(f"Error: Failed to parse JSON: {e}")
            return

    # Map of component directory -> list of entries
    components = {}
    
    # We look for components in sgrn/
    sgrn_dir = os.path.join(project_root, 'sgrn')
    if os.path.exists(sgrn_dir):
        for entry in os.listdir(sgrn_dir):
            if os.path.isdir(os.path.join(sgrn_dir, entry)):
                components[os.path.join('sgrn', entry)] = []

    # Also include utils and sdk if they are top-level or handled differently
    # Based on the structure, they are under sgrn/
    
    # Categorize entries
    for entry in data:
        file_path = entry.get('file', '')
        # Make path relative to project root for easier matching
        rel_path = os.path.relpath(file_path, project_root)
        
        found = False
        for comp_dir in components:
            if rel_path.startswith(comp_dir):
                components[comp_dir].append(entry)
                found = True
                break
        
        # If not found in a specific component, maybe it's in a shared area?
        # For now, we only care about splitting sgrn/ components

    # Write out the split files
    for comp_dir, entries in components.items():
        if not entries:
            continue
            
        target_path = os.path.join(project_root, comp_dir, 'compile_commands.json')
        print(f"Writing {len(entries)} entries to {target_path}...")
        with open(target_path, 'w') as f:
            json.dump(entries, f, indent=2)

    # Also create/update root compile_commands.json
    root_cc = os.path.join(project_root, 'compile_commands.json')
    print(f"Updating root compile_commands.json with {len(data)} entries...")
    with open(root_cc, 'w') as f:
        json.dump(data, f, indent=2)

if __name__ == "__main__":
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
    # Try different possible build locations
    possible_masters = [
        os.path.join(root, 'build', 'linux', 'compile_commands.json'),
        os.path.join(root, 'build', 'compile_commands.json'),
    ]
    
    master = None
    for p in possible_masters:
        if os.path.exists(p):
            master = p
            break
            
    if not master:
        print("Could not find master compile_commands.json in build/linux or build/")
        sys.exit(1)
        
    split_compile_commands(master, root)
