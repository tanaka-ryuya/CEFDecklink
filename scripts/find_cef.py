import json
import sys

platform = 'windows64'
file_type = 'minimal'

if len(sys.argv) > 1:
    platform = sys.argv[1]
if len(sys.argv) > 2:
    file_type = sys.argv[2]

try:
    with open('cef_index.json', 'r') as f:
        data = json.load(f)
    
    if platform not in data:
        print(f"Error: {platform} key not found", file=sys.stderr)
        sys.exit(1)
        
    versions = data[platform]['versions']
    
    target_url = None
    
    # helper to find file in version
    def find_file(v):
        for f in v['files']:
            if f['type'] == file_type:
                return f['name']
        return None

    # First pass: Stable
    for v in versions:
        if v.get('channel') == 'stable':
            name = find_file(v)
            if name:
                target_url = name
                break
                
    # Second pass: Beta (if no stable)
    if not target_url:
        for v in versions:
            if v.get('channel') == 'beta':
                name = find_file(v)
                if name:
                    target_url = name
                    break
    
    if target_url:
        final_url = f"https://cef-builds.spotifycdn.com/{target_url.replace('+', '%2B')}"
        print(final_url)
    else:
        print(f"Error: No suitable build found for platform={platform}, type={file_type}", file=sys.stderr)
        sys.exit(1)

except Exception as e:
    print(f"Error: {e}", file=sys.stderr)
    sys.exit(1)
