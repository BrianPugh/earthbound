
;returns channel 2 if channel 1 == channel 2
;returns channel 1 if channel2 > channel1 and channel2 - channel1 <= 6
;returns (channel 2) + 6 if channel2 > channel1 and channel2 - channel1 > 6
;returns (channel 2) - 6 if channel2 < channel1 and channel1 - channel2 <= 6
;returns channel 1 if channel2 < channel1 and channel1 - channel2 > 6
ADJUST_SINGLE_COLOUR:
	BEGIN_C_FUNCTION
	STACK_RESERVE_VARS
	STACK_RESERVE_INT16
	STACK_RESERVE_PARAM_INT16 "COLOUR" ;int
	STACK_RESERVE_PARAM_INT16 "COLOUR2" ;int
	STACK_RESERVE_RETURN_INT16
	END_STACK_VARS
	STX @VIRTUAL02
	STA @LOCAL00
	CMP @VIRTUAL02
	BNE @NOT_EQUAL ;channel 1 != channel 2
	LDA @VIRTUAL02
	BRA @RETURN
@NOT_EQUAL:
	CMP @VIRTUAL02
	BLTEQ @C1_LESS_EQUAL ;channel1 <= channel 2
	SEC
	SBC @VIRTUAL02
	CMP #6
	BLTEQ @RETURN_C2_LOWER ;channel1 - channel2 <= 6
	LDA @LOCAL00
	SEC
	SBC #6
	STA @VIRTUAL02
@RETURN_C2_LOWER:
	LDA @VIRTUAL02
	BRA @RETURN
@C1_LESS_EQUAL:
	STA @VIRTUAL04
	LDA @VIRTUAL02
	SEC
	SBC @VIRTUAL04
	CMP #6
	BLTEQ @RETURN_C2_UPPER ;channel2 - channel1 <= 6
	LDA @LOCAL00
	CLC
	ADC #6
	STA @VIRTUAL02
@RETURN_C2_UPPER:
	LDA @VIRTUAL02
@RETURN:
	END_C_FUNCTION
