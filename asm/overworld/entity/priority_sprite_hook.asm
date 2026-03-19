
; No-op hook called between priority sprite render passes.
; RENDER_ALL_PRIORITY_SPRITES checks UNUSED_7E2402 against each
; priority level and calls this stub when matched. Since the check
; variable is unused, this is effectively dead code.
PRIORITY_SPRITE_HOOK:
	RTS
