# Feed the kernel's inquiries, as they are, to a small model and print what it makes of them.
# usage: python probe.py [jev|local] [poll_seconds]      jev reads AI_GATEWAY_API_KEY; local needs torch + transformers + CUDA
import json, os, subprocess, sys, time, urllib.request

USER = os.getlogin()
KERNEL = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'build', 'Release', 'kernel.exe')
BORING = [' boring', ' generic', ' routine', ' ordinary', ' empty', ' nothing', ' mundane']
NOTABLE = [' interesting', ' funny', ' notable', ' specific', ' striking', ' amusing', ' embarrassing']

QUESTIONS = {
    'jev': {  # TypeSafe's atomic questions: narrow and literal, each about a named part of the state
        'remarkable': {
            'type': 'boolean',
            'instructions': f"Is there something specific in `focus` that a friend sitting beside {USER} would naturally blurt a comment about?",
            'criteria': {
                'true': 'It shows something specific and comment-worthy: a particular video, song, game, product, search query, error message, a funny or embarrassing title.',
                'false': 'It is generic or routine: an empty desktop, a file manager, a new tab, an app name with no specific content.',
            },
        },
        'busy': {
            'type': 'score',
            'instructions': f'How much would an interruption cost {USER} right now, judging by `focus`?',
            'criteria': [
                'Idle or passive leisure: a desktop or launcher with nothing open, music playing, a video or stream, scrolling a feed.',
                'Casual activity: browsing the web, shopping, chatting with friends, changing settings, managing files.',
                'Focused work: writing or debugging code, writing a document, coursework, reading documentation or a paper, a spreadsheet.',
                'Must not be interrupted: a call or meeting, a presentation or screen share, an exam or timed quiz, a competitive online match.',
            ],
        },
    },
    'local': {  # each answer continues `prefill`; we compare the logits of the label words, low to high
        'remarkable': {
            'ask': f'In one word, would a friend sitting beside {USER} find what is in `focus` worth a comment?',
            'prefill': f"What is on {USER}'s screen is",
            'labels': [BORING, NOTABLE],
        },
        'busy': {
            'ask': f'In one word, what is {USER} doing right now, judging by `focus`?',
            'prefill': f'Right now {USER} is',
            'labels': [
                [' idle', ' away', ' relaxing', ' watching', ' listening', ' resting'],
                [' browsing', ' chatting', ' shopping', ' scrolling', ' searching'],
                [' working', ' coding', ' studying', ' writing', ' reading', ' programming', ' debugging'],
                [' presenting', ' meeting', ' competing', ' testing'],
            ],
        },
    },
}


def jev(state, questions):  # the wire format of the AI SDK's experimental_evaluate (@ai-sdk/gateway 4.0.87)
    request = urllib.request.Request('https://ai-gateway.vercel.sh/v4/ai/evaluation-model', json.dumps({'state': state, 'questions': questions}).encode(), {
        'Content-Type': 'application/json', 'Authorization': 'Bearer ' + os.environ['AI_GATEWAY_API_KEY'],
        'ai-gateway-protocol-version': '0.0.1', 'ai-gateway-auth-method': 'api-key',
        'ai-evaluation-model-specification-version': '4', 'ai-model-id': 'typesafe-ai/jev',
    })
    answers = json.load(urllib.request.urlopen(request, timeout=10))['answers']
    return {name: a['probability'] if a['type'] == 'boolean' else a['score'] for name, a in answers.items()}


def local():  # Aisling's brain.py: one forward pass per question, no generation
    import functools, torch
    from transformers import AutoModelForCausalLM, AutoTokenizer
    tok = AutoTokenizer.from_pretrained('Qwen/Qwen3-0.6B')
    model = AutoModelForCausalLM.from_pretrained('Qwen/Qwen3-0.6B', dtype=torch.bfloat16).cuda().eval()

    @functools.lru_cache(None)
    def token(word):
        ids = tok.encode(word, add_special_tokens=False)
        return ids[0] if len(ids) == 1 else None  # label words must be single tokens

    @torch.no_grad()
    def answer(state, q):
        msgs = [{'role': 'system', 'content': 'You describe what is happening on a PC from a JSON state. Be literal.'},
                {'role': 'user', 'content': 'STATE:\n' + json.dumps(state, ensure_ascii=False) + '\n\n' + q['ask']}]
        text = tok.apply_chat_template(msgs, tokenize=False, add_generation_prompt=True, enable_thinking=False) + q['prefill']
        logits = model(**tok(text, return_tensors='pt').to('cuda')).logits[0, -1].float()
        groups = [[t for t in map(token, words) if t is not None] for words in q['labels']]
        p = torch.softmax(torch.stack([torch.logsumexp(logits[g], 0) for g in groups]), 0).tolist()
        return p[1] if len(p) == 2 else p

    return lambda state, questions: {name: answer(state, q) for name, q in questions.items()}


backend = sys.argv[1] if len(sys.argv) > 1 else 'jev'
ask = local() if backend == 'local' else jev
show = lambda v: [round(x, 2) for x in v] if isinstance(v, list) else round(v, 2)
kernel = subprocess.Popen([KERNEL, *sys.argv[2:3]], stdout=subprocess.PIPE, encoding='utf-8')
for line in kernel.stdout:
    state = json.loads(line)
    del state['t']  # a clock reading says nothing to a model
    start = time.perf_counter()
    answers = ask(state, QUESTIONS[backend])
    print(line.strip(), '\n   ', {name: show(v) for name, v in answers.items()}, f'{(time.perf_counter() - start) * 1000:.0f} ms', flush=True)
