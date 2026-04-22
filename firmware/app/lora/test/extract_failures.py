import json
import html
import re

with open('test_report.html', 'r', encoding='utf-8') as f:
    content = f.read()

match = re.search(r'data-jsonblob="([^"]+)"', content)
if match:
    json_str = html.unescape(match.group(1))
    data = json.loads(json_str)
    for test_id, results in data['tests'].items():
        for r in results:
            if r['result'] == 'Failed':
                print(f"\n=== {test_id} ===")
                log = r.get('log', 'No log')
                print(log[:3000])
