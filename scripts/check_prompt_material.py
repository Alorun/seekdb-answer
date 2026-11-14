#!/usr/bin/env python3
"""Check official JSON constraints and evidence links without assigning scores."""
import argparse
import hashlib
import json
from pathlib import Path
import re


def pairs(items):
    out = {}
    for key, value in items:
        if key in out:
            raise ValueError(f'duplicate JSON key: {key}')
        out[key] = value
    return out


def resolve(data, pointer):
    if pointer == '':
        return data
    if not isinstance(pointer, str) or not pointer.startswith('/'):
        raise ValueError(f'invalid pointer: {pointer!r}')
    node = data
    for token in pointer[1:].split('/'):
        if re.search(r'~(?![01])', token):
            raise ValueError(f'invalid escape: {pointer}')
        token = token.replace('~1', '/').replace('~0', '~')
        if isinstance(node, list):
            if not re.fullmatch(r'0|[1-9][0-9]*', token):
                raise ValueError(f'invalid index: {pointer}')
            node = node[int(token)]
        else:
            node = node[token]
    return node


def check(path):
    raw = path.read_bytes()
    if len(raw) > 128 * 1024:
        raise ValueError('material exceeds official 128 KiB limit')
    data = json.loads(raw.decode('utf-8'), object_pairs_hook=pairs,
                      parse_constant=lambda x: (_ for _ in ()).throw(ValueError(x)))
    if not isinstance(data, dict):
        raise ValueError('top level must be object')

    def walk(node):
        if isinstance(node, dict):
            for key, value in node.items():
                if key.endswith('_pointer'):
                    target = resolve(data, value)
                    if not isinstance(target, str):
                        raise ValueError(f'citation must target string: {value}')
                walk(value)
        elif isinstance(node, list):
            for value in node:
                walk(value)
    walk(data)
    for section in ['conversations', 'experiments', 'user_prompts', 'units', 'historical_context_index']:
        ids = [x['id'] for x in data.get(section, [])]
        if len(ids) != len(set(ids)):
            raise ValueError(f'duplicate id in {section}')
    for unit in data.get('units', []):
        for kind in ['student', 'result']:
            evidence = unit.get(kind + '_evidence')
            pointer = unit.get(kind + '_pointer')
            if evidence is not None:
                if not isinstance(evidence, str) or len(evidence) > 240:
                    raise ValueError('citation must be <=240 characters')
                if evidence and evidence not in resolve(data, pointer):
                    raise ValueError('citation is not exact substring at pointer')
    prompt = data.get('development_prompt', {}).get('content')
    if prompt is not None:
        expected = Path(__file__).resolve().parents[1] / 'agent/development_prompt.md'
        if prompt != expected.read_text():
            raise ValueError('embedded development prompt differs from source')
    print(f'PASS JSON/size/IDs/pointers/excerpts: {len(raw)} bytes sha256={hashlib.sha256(raw).hexdigest()}')
    print('Structural validation only; no authenticity certification, semantic scoring, or platform admission.')


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('path', type=Path)
    check(ap.parse_args().path)
