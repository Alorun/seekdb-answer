#!/usr/bin/env python3
"""Build an honest, reproducible evidence candidate; print an apply_patch patch.

Does not assign scores or manufacture student explanations. All new results
are captured from actual subprocesses. Historical normalization is disclosed.
"""
import argparse
import copy
import datetime
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
BASELINE = '99fda4fd4ad09344a3cc7c571dbc931c6e82012a'
RAW_HISTORY = 'dd82df996'


def git(*args):
    return subprocess.check_output(['git', *args], cwd=ROOT, text=True)


def build(verify):
    data = json.loads(git('show', BASELINE + ':agent/conversation.json'))
    raw = json.loads(git('show', RAW_HISTORY + ':agent/conversation.json'))
    data.pop('verified_at', None)
    data.pop('commit', None)
    data['assembled_at'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    data['code_snapshot'] = {
        'base_commit': git('rev-parse', 'HEAD').strip(),
        'last_source_commit': git('log', '-1', '--format=%H', '--', 'src').strip(),
        'files_sha256': {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                         for p in sorted((ROOT / 'src').rglob('*')) if p.is_file()},
        'scope': '本次仅修改 prompt/材料/校验工具，不修改数据库源码；正式提交仍须锁定实际 commit 与材料摘要。',
    }
    data['status'] = {
        'material': '待学生补充真实解释和正式评审的候选；未声称达到目标分数。',
        'historical_baseline': '仓库台账记载 954f61118 材料获平台 12.5；本次不重新认定历史成绩。',
        'formal_admission': '本次未提交平台；历史准入不外推至本候选。',
        'official_protocol': 'JSONL；已核对官方 STDIO_PROTOCOL.md，与题目概述中非 JSON 封装描述存在差异，以冻结题包验证。',
    }
    data['material_notes'] = ('历史版本包含 Agent 归一化任务摘要和技术整理稿，缺少可定位的学生逐条确认记录。'
        '本候选保留其内容，但明确标为历史未核实归属摘要，不作为已证实的学生技术理解。'
        '另保留旧 commit 中标为用户原文节选的记录，来源为 git 归档而非本次原始会话导出。'
        '新 prompt 是本次 Agent 拟定，不能倒填成历史实际使用提示词；新实验由工具实际执行。'
        '没有编造交互、实验或自评分。未确认归属不等于认定历史材料造假。')
    data['user_prompts'] = copy.deepcopy(raw['user_prompts'])
    for p in data['user_prompts']:
        p['source'] = 'git dd82df996:agent/conversation.json 中标记的用户原文节选；本次未获得完整原始会话'
    data['user_prompts'].append({
        'id': 'P_CURRENT', 'source': '2026-11-14 本次用户原文，完整消息',
        'content': '现在你先仔细阅读/home/aln/文档/db.md关于agent prompt的部分，这部分的分数现在一直很低没有找到突破的方案，然后你阅读当前目录下关于prompt几次关键改动和得分和无效的那些关键情况；阅读之后你来修改和优化目前的prompt，目标就是30分以上，现在最高12.5已经卡了太久了且还有很多次无效没有成绩，所以一定要注意规则和官方的规范，还有就是证据充足，要使当前的提示词能够生成现在这些对应的代码',
    })
    for i, c in enumerate(data['conversations']):
        c['id'] = f'H{i + 1:02d}'
        c['role'] = 'historical_summary'
        c['provenance'] = '历史 Agent 整理的摘要，学生归属尚无本地原始记录/逐条确认可核对；不得按学生原话引用。'
        c.pop('prompt_ref', None)
        c.pop('note', None)
    # Keep historical context, fix mismatched links, remove unsupported scoring.
    result_indices = [None, 2, 1, 21, 8, 20, 3, 13, 14, 15, 16, 17, 18]
    links = []
    for u, result_index in zip(data.pop('units'), result_indices):
        link = {'id': u['id'], 'topic': data['conversations'][int(u['student_pointer'].split('/')[2])]['content'],
                'context_pointer': u['student_pointer'],
                'eligibility': '仅作历史技术上下文索引；学生归属未核实，不申请据此计分。'}
        if result_index is not None:
            link['result_pointer'] = f'/experiments/{result_index}/output'
        links.append(link)
    data['historical_context_index'] = links
    for i, e in enumerate(data['experiments']):
        e['id'] = f'HIST_E{i + 1:02d}'
        e['provenance'] = '历史归档摘录/摘要，非本次执行；输出未重新认证为逐字原始日志。'
        if e.get('analysis') == '真实本地/平台结果，作为证据。':
            e['analysis'] = '历史结论摘要；缺少完整命令/环境，不作为本次已复现结果。'
        e.pop('raw_output', None)  # Legacy cross-topic fields were not raw logs.
    for d in data['decisions']:
        d['by'] = 'historical_summary_unverified_attribution'
        d['evidence'] = '历史技术摘要；不得由旧编号推断学生贡献或当前实现状态。'
    data['decisions'] = [d for d in data['decisions'] if d['id'] != 'D10']
    data['development_prompt'] = {
        'role': 'assistant', 'usage_status': '本次新拟定，供后续实际使用；非历史 prompt',
        'content': (ROOT / 'agent/development_prompt.md').read_text(),
    }
    data['agent_analysis'] = [
        {'id': 'A1', 'role': 'assistant', 'content': '旧 U12 讨论 int8/4-bit/PQ/HNSW，却指向 prompt 得分摘要；旧 U13 的 T/B/M 取舍指向 AVX-512 平台下降。已分别改连对应负结果和 T/B/M 实验。'},
        {'id': 'A2', 'role': 'assistant', 'content': '旧 U6 标为学生证据，但对应会话 note 明写“角色说明，非学生证据”，评分理由又讨论未引用的 AVX2/DDL 反例。不能靠重写 reason 把归属缺失变成学生理解。'},
        {'id': 'A3', 'role': 'assistant', 'content': '官方原始分不封顶，每条独立实例最多2分；30原始分至少需要15个满档真实独立实例。13个实例最多26，不可能靠保留13个自评分19.5实现30原始分；若30指排名折算分则不能由材料自行保证。'},
        {'id': 'A4', 'role': 'assistant', 'content': '无效记录是待复核/范围/摘要等合并错误，缺少三轮报告和实例引用，不能证明18个单元为平台数量上限。只保留历史相关性，须获得材料类型、逐维三轮、引用校验和复核原因才能归因。'},
    ]
    data['units'] = []
    data['evidence_policy'] = '不自行评分。技术上下文索引不等于可计分学生实例；后续有真实学生回答、实验与反馈后，再按独立问题组织引用。'
    data['retrospective'] = '本次 Agent 审计纠正了来源归属、结果错配、重复编号和当前/历史状态混用。新 prompt 对齐当前代码路径，但尚未开展从空目录按此 prompt 重建的实验，不主张可逐字复现源码。测试结果只证明所注明覆盖范围，不证明学生已解释或掌握机制。'
    data['limitations'] = [
        '缺少历史完整学生原始会话和逐条确认记录；技术摘要不能替代学生自身解释。',
        '未获取历史各次正式 prompt 三轮评分报告/材料类型/复核原因，不能确定12.5分或无效的唯一根因。',
        '新开发 prompt 尚无从零生成代码的一致性实验；目前只检查模块覆盖与当前源码一致。',
        '本次无新平台提交、排名或得分；不承诺30分。',
        '本次未重测 Docker、ASAN、正式隐藏用例或cgroup性能；本地结果不能外推。',
        '磁盘路径仍保留PK偏移索引，不能据强制磁盘的小数据通过断言任意超内存场景合规。',
        '历史微基准包含未入库/tmp脚本，缺少环境与方差；不同数字不可直接比较。',
    ]
    jobs = [
        ('BUILD', ['cmake', '--build', 'build', '-j2'], '当前Release构建'),
        ('PUBLIC_TEXT', ['python3', 'tests/run_public_cases.py', '--bin', './build/minivectordb'], '本地text profile公开20例，不替代正式JSONL'),
        ('SQL_EDGES', ['python3', 'tests/run_sql_edges.py', '--bin', './build/minivectordb'], '仅当前build，内存与强制磁盘各158检查'),
        ('ORACLE_UNIFORM', ['python3', 'tests/run_recall.py', '--bin', './build/minivectordb', '--rows', '5000', '--queries', '30'], '固定生成器种子；uniform精确差分、ann、失效与回滚'),
        ('ORACLE_CLUSTERED', ['python3', 'tests/run_recall.py', '--bin', './build/minivectordb', '--rows', '5000', '--queries', '30', '--clustered'], '迁移到clustered分布，结果不代表所有分布'),
        ('CRASH', ['python3', 'tests/run_crash_fuzz.py', './build/minivectordb', '--iterations', '100', '--seed', '20261114'], '内存/磁盘各100轮，六个确定性点与随机强杀'),
        ('CHECKPOINT', ['python3', 'tests/run_checkpoint.py', '--bin', './build/minivectordb'], '10k批量记录、阈值1、关停checkpoint重启；不等于逐行完整性oracle'),
    ]
    if verify:
        for name, argv, scope in jobs:
            print('running ' + name, file=sys.stderr, flush=True)
            r = subprocess.run(argv, cwd=ROOT, text=True, capture_output=True, timeout=180)
            data['experiments'].append({
                'id': 'CURRENT_' + name, 'role': 'tool',
                'command': shlex.join(argv), 'scope': scope,
                'exit_code': r.returncode, 'output': r.stdout, 'stderr': r.stderr,
                'provenance': '本次工具实跑原文；不是学生发言，不自动构成学生学习实例。',
            })
            if r.returncode:
                raise RuntimeError(f'{name} failed: {r.stdout} {r.stderr}')
    data['summary'] = '真实来源分层候选：历史用户节选、未核实归属的历史技术摘要、本次Agent开发prompt与实际工具输出；去掉自评分，不伪造学生学习闭环。'
    data['material_summary'] = data['summary']
    return data


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--verify', action='store_true', help='actually run all listed local tests')
    args = ap.parse_args()
    data = build(args.verify)
    content = json.dumps(data, ensure_ascii=False, indent=2, allow_nan=False) + '\n'
    if len(content.encode()) > 128 * 1024:
        raise ValueError('material exceeds official 128 KiB limit')
    lines = ['*** Begin Patch']
    relative = 'agent/candidates/conversation.audit.json'
    if (ROOT / relative).exists():
        previous = (ROOT / relative).read_text()
        lines.extend(['*** Update File: ' + relative, '@@'])
        lines.extend('-' + line for line in previous.splitlines())
    else:
        lines.append('*** Add File: ' + relative)
    lines.extend('+' + line for line in content.splitlines())
    lines.append('*** End Patch')
    print('\n'.join(lines))


if __name__ == '__main__':
    main()
