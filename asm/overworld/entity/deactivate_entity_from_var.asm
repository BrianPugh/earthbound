
; Deactivate the entity whose offset is stored in the current entity's
; var[N], where N is read from the action script stream.
; Entry point 1 (near): reads var index, looks up var value, deactivates.
DEACTIVATE_ENTITY_FROM_VAR:
	JSR SCRIPT_READ_BYTE
	STY $94
	ASL
	TAX
	LDA ENTITY_SCRIPT_VAR_TABLES,X
	CLC
	ADC $88
	TAX
	LDA __BSS_START__,X
	TAX
	JMP DEACTIVATE_ENTITY
; Entry point 2 (far): reads entity offset directly from script, deactivates.
DEACTIVATE_ENTITY_BY_SCRIPT_PARAM:
	JSR SCRIPT_READ_BYTE
	STY $94
	TAX
	JSR DEACTIVATE_ENTITY
	RTL
