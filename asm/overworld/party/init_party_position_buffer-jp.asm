
INIT_PARTY_POSITION_BUFFER:
	REP #PROC_FLAGS::ACCUM8 | PROC_FLAGS::INDEX8 | PROC_FLAGS::CARRY
	STACK_RESERVE_VARS
	END_STACK_VARS
	STZ GAME_STATE + game_state::unknown88
	LDX #.LOWORD(PLAYER_POSITION_BUFFER)
	LDY #$0002
@FILL_BUFFER_ENTRY:
	LDA GAME_STATE+game_state::leader_x_coord
	STA a:player_position_buffer_entry::x_coord,X
	LDA GAME_STATE+game_state::leader_y_coord
	STA a:player_position_buffer_entry::y_coord,X
	LDA GAME_STATE+game_state::leader_direction
	STA a:player_position_buffer_entry::direction,X
	LDA GAME_STATE+game_state::walking_style
	STA a:player_position_buffer_entry::walking_style,X
	LDA GAME_STATE+game_state::trodden_tile_type
	STA a:player_position_buffer_entry::tile_flags,X
	STZ PLAYER_MOVEMENT_FLAGS
	STZ a:player_position_buffer_entry::unknown10,X
	TXA
	CLC
	ADC #.SIZEOF(player_position_buffer_entry) * 255 ;last entry
	TAX
	DEY
	BNE @FILL_BUFFER_ENTRY
	LDY #$0000
	BRA @MEMBER_LOOP_CHECK
@INIT_MEMBER:
	TYA
	CLC
	ADC #.LOWORD(GAME_STATE)
	TAX
	LDA a:game_state::player_controlled_party_members,X
	AND #$00FF
	ASL
	TAX
	LDA CHOSEN_FOUR_PTRS,X
	TAX
	STZ a:char_struct::position_index,X
	LDA #$FFFF
	STA a:char_struct::buffer_walking_style,X
	STA a:char_struct::previous_walking_style,X
	TYA
	ASL
	CLC
	ADC #.LOWORD(GAME_STATE)
	TAX
	LDA a:game_state::party_entity_slots,X
	ASL
	TAX
	LDA GAME_STATE+game_state::leader_x_coord
	STA ENTITY_ABS_X_TABLE,X
	LDA GAME_STATE+game_state::leader_y_coord
	STA ENTITY_ABS_Y_TABLE,X
	LDA GAME_STATE+game_state::leader_direction
	STA ENTITY_DIRECTIONS,X
	LDA GAME_STATE+game_state::trodden_tile_type
	STA ENTITY_SURFACE_FLAGS,X
	INY
@MEMBER_LOOP_CHECK:
	LDA GAME_STATE+game_state::party_count
	AND #$00FF
	STA @VIRTUAL02
	TYA
	CMP @VIRTUAL02
	BCC @INIT_MEMBER
	PLD
	RTL
