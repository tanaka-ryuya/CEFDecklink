import json
import sys

try:
    print("Loading JSON...")
    with open('cef_index.json', 'r') as f:
        data = json.load(f)
    
    print("JSON Loaded. Searching for windows64...")
    if 'windows64' not in data:
        print("Error: windows64 key not found")
        sys.exit(1)
        
    versions = data['windows64']['versions']
    print(f"Found {len(versions)} versions for windows64")
    
    target_url = None
    
    # helper to find file in version
    def find_file(v):
        for f in v['files']:
            if f['type'] == 'standard':
                return f['name']
        return None

    # First pass: Stable
    for v in versions:
        if v.get('channel') == 'stable':
            name = find_file(v)
            if name:
                target_url = name
                print(f"Found stable version: {v['cef_version']}")
                break
                
    # Second pass: Beta (if no stable)
    if not target_url:
        print("No stable build found. Checking beta...")
        for v in versions:
            if v.get('channel') == 'beta':
                name = find_file(v)
                if name:
                    target_url = name
                    print(f"Found beta version: {v['cef_version']}")
                    break
    
    if target_url:
        # URL encode + if needed, but usually spotify cdn handles it or it's +
        # The index.html has a replaceAll(url, '+', '%2B');
        final_url = f"https://cef-builds.spotifycdn.com/{target_url.replace('+', '%2B')}"
        print(f"DOWNLOAD_URL={final_url}")
    else:
        print("Error: No suitable build found")

except Exception as e:
    print(f"Error: {e}")
