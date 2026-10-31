
SCREEN_TRANSITION_CONFIG_TABLE:
	.BYTE $00 ;Duration
	.BYTE $00 ;Animation ID
	.BYTE $00 ;Animation flags
	.BYTE $00 ;Fade style
	.BYTE $00 ;Direction
	.WORD $0000 ;Slide speed
	.BYTE SFX::NONE ;Sound effect to play at beginning
	.BYTE $00 ;Secondary duration
	.BYTE $00 ;Secondary animation ID
	.BYTE $00 ;Secondary animation flags
	.BYTE SFX::NONE ;Sound effect to play at end

	.BYTE $14 ;Duration
	.BYTE $00 ;Animation ID
	.BYTE $00 ;Animation flags
	.BYTE $00 ;Fade style
	.BYTE $00 ;Direction
	.WORD $0000 ;Slide speed
	.BYTE SFX::NONE ;Sound effect to play at beginning
	.BYTE $14 ;Secondary duration
	.BYTE $00 ;Secondary animation ID
	.BYTE $00 ;Secondary animation flags
	.BYTE SFX::NONE ;Sound effect to play at end

	.BYTE $14 ;Duration
	.BYTE $00 ;Animation ID
	.BYTE $00 ;Animation flags
	.BYTE $64 ;Fade style
	.BYTE $00 ;Direction
	.WORD $0000 ;Slide speed
	.BYTE SFX::NONE ;Sound effect to play at beginning
	.BYTE $3C ;Secondary duration
	.BYTE $00 ;Secondary animation ID
	.BYTE $00 ;Secondary animation flags
	.BYTE SFX::NONE ;Sound effect to play at end

	.BYTE $14 ;Duration
	.BYTE $00 ;Animation ID
	.BYTE $00 ;Animation flags
	.BYTE $00 ;Fade style
	.BYTE $00 ;Direction
	.WORD $0000 ;Slide speed
	.BYTE SFX::NONE ;Sound effect to play at beginning
	.BYTE $14 ;Secondary duration
	.BYTE $00 ;Secondary animation ID
	.BYTE $00 ;Secondary animation flags
	.BYTE SFX::NONE ;Sound effect to play at end

	.BYTE $14 ;Duration
	.BYTE $00 ;Animation ID
	.BYTE $00 ;Animation flags
	.BYTE $00 ;Fade style
	.BYTE $00 ;Direction
	.WORD $0000 ;Slide speed
	.BYTE SFX::NONE ;Sound effect to play at beginning
	.BYTE $14 ;Secondary duration
	.BYTE $00 ;Secondary animation ID
	.BYTE $00 ;Secondary animation flags
	.BYTE SFX::DOOR_CLOSE ;Sound effect to play at end

	.BYTE $1E ;Duration
	.BYTE $00 ;Animation ID
	.BYTE $00 ;Animation flags
	.BYTE $00 ;Fade style
	.BYTE $00 ;Direction
	.WORD $1000 ;Slide speed
	.BYTE SFX::FALLING ;Sound effect to play at beginning
	.BYTE $14 ;Secondary duration
	.BYTE $00 ;Secondary animation ID
	.BYTE $00 ;Secondary animation flags
	.BYTE SFX::TOOK_DAMAGE ;Sound effect to play at end

	.BYTE $14 ;Duration
	.BYTE $00 ;Animation ID
	.BYTE $00 ;Animation flags
	.BYTE $00 ;Fade style
	.BYTE $00 ;Direction
	.WORD $0000 ;Slide speed
	.BYTE SFX::NONE ;Sound effect to play at beginning
	.BYTE $14 ;Secondary duration
	.BYTE $00 ;Secondary animation ID
	.BYTE $00 ;Secondary animation flags
	.BYTE SFX::AREA_TRANSITION ;Sound effect to play at end

	.BYTE $14 ;Duration
	.BYTE $00 ;Animation ID
	.BYTE $00 ;Animation flags
	.BYTE $00 ;Fade style
	.BYTE $00 ;Direction
	.WORD $0000 ;Slide speed
	.BYTE SFX::DOOR_OPEN ;Sound effect to play at beginning
	.BYTE $14 ;Secondary duration
	.BYTE $00 ;Secondary animation ID
	.BYTE $00 ;Secondary animation flags
	.BYTE SFX::DOOR_CLOSE ;Sound effect to play at end

	.BYTE $14 ;Duration
	.BYTE $00 ;Animation ID
	.BYTE $00 ;Animation flags
	.BYTE $00 ;Fade style
	.BYTE $00 ;Direction
	.WORD $0000 ;Slide speed
	.BYTE SFX::DOOR_OPEN ;Sound effect to play at beginning
	.BYTE $3C ;Secondary duration
	.BYTE $00 ;Secondary animation ID
	.BYTE $00 ;Secondary animation flags
	.BYTE SFX::DOOR_CLOSE ;Sound effect to play at end

	.BYTE $14 ;Duration
	.BYTE $00 ;Animation ID
	.BYTE $00 ;Animation flags
	.BYTE $00 ;Fade style
	.BYTE $00 ;Direction
	.WORD $0000 ;Slide speed
	.BYTE SFX::DOOR_OPEN ;Sound effect to play at beginning
	.BYTE $3C ;Secondary duration
	.BYTE $00 ;Secondary animation ID
	.BYTE $00 ;Secondary animation flags
	.BYTE SFX::DOOR_CLOSE ;Sound effect to play at end

	.BYTE $14 ;Duration
	.BYTE $00 ;Animation ID
	.BYTE $00 ;Animation flags
	.BYTE $00 ;Fade style
	.BYTE $00 ;Direction
	.WORD $0000 ;Slide speed
	.BYTE SFX::DOOR_OPEN ;Sound effect to play at beginning
	.BYTE $14 ;Secondary duration
	.BYTE $00 ;Secondary animation ID
	.BYTE $00 ;Secondary animation flags
	.BYTE SFX::NONE ;Sound effect to play at end

	.BYTE $64 ;Duration
	.BYTE $05 ;Animation ID
	.BYTE $00 ;Animation flags
	.BYTE $31 ;Fade style
	.BYTE $00 ;Direction
	.WORD $0000 ;Slide speed
	.BYTE SFX::NONE ;Sound effect to play at beginning
	.BYTE $64 ;Secondary duration
	.BYTE $05 ;Secondary animation ID
	.BYTE $00 ;Secondary animation flags
	.BYTE SFX::NONE ;Sound effect to play at end

	.BYTE $14 ;Duration
	.BYTE $00 ;Animation ID
	.BYTE $00 ;Animation flags
	.BYTE $00 ;Fade style
	.BYTE $00 ;Direction
	.WORD $0000 ;Slide speed
	.BYTE SFX::AREA_TRANSITION ;Sound effect to play at beginning
	.BYTE $14 ;Secondary duration
	.BYTE $00 ;Secondary animation ID
	.BYTE $00 ;Secondary animation flags
	.BYTE SFX::NONE ;Sound effect to play at end

	.BYTE $14 ;Duration
	.BYTE $00 ;Animation ID
	.BYTE $00 ;Animation flags
	.BYTE $00 ;Fade style
	.BYTE $00 ;Direction
	.WORD $0000 ;Slide speed
	.BYTE SFX::AREA_TRANSITION ;Sound effect to play at beginning
	.BYTE $14 ;Secondary duration
	.BYTE $00 ;Secondary animation ID
	.BYTE $00 ;Secondary animation flags
	.BYTE SFX::AREA_TRANSITION ;Sound effect to play at end

	.BYTE $14 ;Duration
	.BYTE $00 ;Animation ID
	.BYTE $00 ;Animation flags
	.BYTE $64 ;Fade style
	.BYTE $00 ;Direction
	.WORD $0000 ;Slide speed
	.BYTE SFX::NONE ;Sound effect to play at beginning
	.BYTE $14 ;Secondary duration
	.BYTE $00 ;Secondary animation ID
	.BYTE $00 ;Secondary animation flags
	.BYTE SFX::NONE ;Sound effect to play at end

	.BYTE $14 ;Duration
	.BYTE $00 ;Animation ID
	.BYTE $00 ;Animation flags
	.BYTE $00 ;Fade style
	.BYTE $00 ;Direction
	.WORD $0000 ;Slide speed
	.BYTE SFX::STAIRS ;Sound effect to play at beginning
	.BYTE $14 ;Secondary duration
	.BYTE $00 ;Secondary animation ID
	.BYTE $00 ;Secondary animation flags
	.BYTE SFX::NONE ;Sound effect to play at end

	.BYTE $FF ;Duration
	.BYTE $02 ;Animation ID
	.BYTE $80 ;Animation flags
	.BYTE $64 ;Fade style
	.BYTE $00 ;Direction
	.WORD $0000 ;Slide speed
	.BYTE SFX::NONE ;Sound effect to play at beginning
	.BYTE $3C ;Secondary duration
	.BYTE $02 ;Secondary animation ID
	.BYTE $01 ;Secondary animation flags
	.BYTE SFX::NONE ;Sound effect to play at end

	.BYTE $3C ;Duration
	.BYTE $00 ;Animation ID
	.BYTE $00 ;Animation flags
	.BYTE $00 ;Fade style
	.BYTE $26 ;Direction
	.WORD $0C00 ;Slide speed
	.BYTE SFX::CASH_REGISTER ;Sound effect to play at beginning
	.BYTE $14 ;Secondary duration
	.BYTE $00 ;Secondary animation ID
	.BYTE $00 ;Secondary animation flags
	.BYTE SFX::NONE ;Sound effect to play at end

	.BYTE $3C ;Duration
	.BYTE $00 ;Animation ID
	.BYTE $00 ;Animation flags
	.BYTE $00 ;Fade style
	.BYTE $07 ;Direction
	.WORD $0C00 ;Slide speed
	.BYTE SFX::CASH_REGISTER ;Sound effect to play at beginning
	.BYTE $14 ;Secondary duration
	.BYTE $00 ;Secondary animation ID
	.BYTE $00 ;Secondary animation flags
	.BYTE SFX::NONE ;Sound effect to play at end

	.BYTE $3C ;Duration
	.BYTE $00 ;Animation ID
	.BYTE $00 ;Animation flags
	.BYTE $00 ;Fade style
	.BYTE $3B ;Direction
	.WORD $0C00 ;Slide speed
	.BYTE SFX::CASH_REGISTER ;Sound effect to play at beginning
	.BYTE $14 ;Secondary duration
	.BYTE $00 ;Secondary animation ID
	.BYTE $00 ;Secondary animation flags
	.BYTE SFX::NONE ;Sound effect to play at end

	.BYTE $3C ;Duration
	.BYTE $00 ;Animation ID
	.BYTE $00 ;Animation flags
	.BYTE $00 ;Fade style
	.BYTE $3D ;Direction
	.WORD $0C00 ;Slide speed
	.BYTE SFX::CASH_REGISTER ;Sound effect to play at beginning
	.BYTE $14 ;Secondary duration
	.BYTE $00 ;Secondary animation ID
	.BYTE $00 ;Secondary animation flags
	.BYTE SFX::NONE ;Sound effect to play at end

	.BYTE $3C ;Duration
	.BYTE $00 ;Animation ID
	.BYTE $00 ;Animation flags
	.BYTE $00 ;Fade style
	.BYTE $3D ;Direction
	.WORD $0C00 ;Slide speed
	.BYTE SFX::CASH_REGISTER ;Sound effect to play at beginning
	.BYTE $14 ;Secondary duration
	.BYTE $00 ;Secondary animation ID
	.BYTE $00 ;Secondary animation flags
	.BYTE SFX::NONE ;Sound effect to play at end

	.BYTE $3C ;Duration
	.BYTE $00 ;Animation ID
	.BYTE $00 ;Animation flags
	.BYTE $00 ;Fade style
	.BYTE $07 ;Direction
	.WORD $0C00 ;Slide speed
	.BYTE SFX::CASH_REGISTER ;Sound effect to play at beginning
	.BYTE $14 ;Secondary duration
	.BYTE $00 ;Secondary animation ID
	.BYTE $00 ;Secondary animation flags
	.BYTE SFX::NONE ;Sound effect to play at end

	.BYTE $3C ;Duration
	.BYTE $00 ;Animation ID
	.BYTE $00 ;Animation flags
	.BYTE $00 ;Fade style
	.BYTE $05 ;Direction
	.WORD $0C00 ;Slide speed
	.BYTE SFX::CASH_REGISTER ;Sound effect to play at beginning
	.BYTE $14 ;Secondary duration
	.BYTE $00 ;Secondary animation ID
	.BYTE $00 ;Secondary animation flags
	.BYTE SFX::NONE ;Sound effect to play at end

	.BYTE $3C ;Duration
	.BYTE $00 ;Animation ID
	.BYTE $00 ;Animation flags
	.BYTE $00 ;Fade style
	.BYTE $30 ;Direction
	.WORD $0C00 ;Slide speed
	.BYTE SFX::CASH_REGISTER ;Sound effect to play at beginning
	.BYTE $14 ;Secondary duration
	.BYTE $00 ;Secondary animation ID
	.BYTE $00 ;Secondary animation flags
	.BYTE SFX::NONE ;Sound effect to play at end

	.BYTE $3C ;Duration
	.BYTE $00 ;Animation ID
	.BYTE $00 ;Animation flags
	.BYTE $00 ;Fade style
	.BYTE $16 ;Direction
	.WORD $0C00 ;Slide speed
	.BYTE SFX::CASH_REGISTER ;Sound effect to play at beginning
	.BYTE $14 ;Secondary duration
	.BYTE $00 ;Secondary animation ID
	.BYTE $00 ;Secondary animation flags
	.BYTE SFX::NONE ;Sound effect to play at end

	.BYTE $3C ;Duration
	.BYTE $00 ;Animation ID
	.BYTE $00 ;Animation flags
	.BYTE $00 ;Fade style
	.BYTE $26 ;Direction
	.WORD $0C00 ;Slide speed
	.BYTE SFX::CASH_REGISTER ;Sound effect to play at beginning
	.BYTE $14 ;Secondary duration
	.BYTE $00 ;Secondary animation ID
	.BYTE $00 ;Secondary animation flags
	.BYTE SFX::NONE ;Sound effect to play at end

	.BYTE $3C ;Duration
	.BYTE $00 ;Animation ID
	.BYTE $00 ;Animation flags
	.BYTE $00 ;Fade style
	.BYTE $1B ;Direction
	.WORD $0C00 ;Slide speed
	.BYTE SFX::CASH_REGISTER ;Sound effect to play at beginning
	.BYTE $14 ;Secondary duration
	.BYTE $00 ;Secondary animation ID
	.BYTE $00 ;Secondary animation flags
	.BYTE SFX::NONE ;Sound effect to play at end

	.BYTE $14 ;Duration
	.BYTE $00 ;Animation ID
	.BYTE $00 ;Animation flags
	.BYTE $00 ;Fade style
	.BYTE $00 ;Direction
	.WORD $0000 ;Slide speed
	.BYTE SFX::CASH_REGISTER ;Sound effect to play at beginning
	.BYTE $14 ;Secondary duration
	.BYTE $00 ;Secondary animation ID
	.BYTE $00 ;Secondary animation flags
	.BYTE SFX::NONE ;Sound effect to play at end

	.BYTE $78 ;Duration
	.BYTE $00 ;Animation ID
	.BYTE $00 ;Animation flags
	.BYTE $64 ;Fade style
	.BYTE $00 ;Direction
	.WORD $0000 ;Slide speed
	.BYTE SFX::NONE ;Sound effect to play at beginning
	.BYTE $64 ;Secondary duration
	.BYTE $00 ;Secondary animation ID
	.BYTE $00 ;Secondary animation flags
	.BYTE SFX::NONE ;Sound effect to play at end

	.BYTE $64 ;Duration
	.BYTE $00 ;Animation ID
	.BYTE $00 ;Animation flags
	.BYTE $64 ;Fade style
	.BYTE $00 ;Direction
	.WORD $0000 ;Slide speed
	.BYTE SFX::MYSTERIOUS_TRANSPORT ;Sound effect to play at beginning
	.BYTE $3C ;Secondary duration
	.BYTE $00 ;Secondary animation ID
	.BYTE $00 ;Secondary animation flags
	.BYTE SFX::NONE ;Sound effect to play at end

	.BYTE $64 ;Duration
	.BYTE $00 ;Animation ID
	.BYTE $00 ;Animation flags
	.BYTE $00 ;Fade style
	.BYTE $10 ;Direction
	.WORD $07E8 ;Slide speed
	.BYTE SFX::GHOST ;Sound effect to play at beginning
	.BYTE $14 ;Secondary duration
	.BYTE $00 ;Secondary animation ID
	.BYTE $00 ;Secondary animation flags
	.BYTE SFX::AFFLICTED ;Sound effect to play at end

	.BYTE $64 ;Duration
	.BYTE $00 ;Animation ID
	.BYTE $00 ;Animation flags
	.BYTE $00 ;Fade style
	.BYTE $30 ;Direction
	.WORD $07E8 ;Slide speed
	.BYTE SFX::GHOST ;Sound effect to play at beginning
	.BYTE $14 ;Secondary duration
	.BYTE $00 ;Secondary animation ID
	.BYTE $00 ;Secondary animation flags
	.BYTE SFX::AFFLICTED ;Sound effect to play at end

	.BYTE $1E ;Duration
	.BYTE $00 ;Animation ID
	.BYTE $00 ;Animation flags
	.BYTE $00 ;Fade style
	.BYTE $00 ;Direction
	.WORD $1000 ;Slide speed
	.BYTE SFX::FALLING ;Sound effect to play at beginning
	.BYTE $14 ;Secondary duration
	.BYTE $00 ;Secondary animation ID
	.BYTE $00 ;Secondary animation flags
	.BYTE SFX::NONE ;Sound effect to play at end
