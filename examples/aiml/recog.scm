;
; recog.scm
;
; This is a rough sketch of the idea that pattern recognition
; is the dual of pattern matching.  There are many things wrong
; with the below; its just a sketch. However, it does work.

(use-modules (opencog))
(use-modules (opencog query))

; Two different pseudo-AIML rules:
;    I * you   --> I * you too
;    I love *  --> I like * a lot!
;
(BindLink
	(ListLink
		(ConceptNode "I")
		(GlobNode "$star")
		(ConceptNode "you"))
	(ListLink
		(ConceptNode "I")
		(GlobNode "$star")
		(ConceptNode "you")
		(ConceptNode "too")))

(BindLink
	(ListLink
		(ConceptNode "I")
		(ConceptNode "love")
		(GlobNode "$star"))
	(ListLink
		(ConceptNode "I")
		(ConceptNode "like")
		(GlobNode "$star")
		(ConceptNode "a")
		(ConceptNode "lot!")))

;-------------------------------------------------------
(define sent
	;; A pretend "sentence" that is the "input".
	(PatternLink
		(BindLink
			(ListLink
				(ConceptNode "I")
				(ConceptNode "love")
				(ConceptNode "you"))
			(VariableNode "$impl"))))

;; Search for patterns that match the sentence. Both of the above
;; should match.
(cog-recognize sent)

;; The above should return the below:
;; The BindLinks are NOT evaluated!  To evaluate, see bottom.
(SetLink
	(BindLink
		(ListLink
			(ConceptNode "I")
			(GlobNode "$star")
			(ConceptNode "you")
		)
		(ListLink
			(ConceptNode "I")
			(GlobNode "$star")
			(ConceptNode "you")
			(ConceptNode "too")
		)
	)
	(BindLink
		(ListLink
			(ConceptNode "I")
			(ConceptNode "love")
			(GlobNode "$star")
		)
		(ListLink
			(ConceptNode "I")
			(ConceptNode "like")
			(GlobNode "$star")
			(ConceptNode "a")
			(ConceptNode "lot!")
		)
	)
)

;-------------------------------------------------------
;; Another sentence, but with adverbs.  It will match one of the
;; patterns, but not the other.
(define adv-sent
	(PatternLink
		(BindLink
			(ListLink
				(ConceptNode "I")
				(ConceptNode "really")
				(ConceptNode "truly")
				(ConceptNode "love")
				(ConceptNode "you"))
			(VariableNode "$impl"))))

;; Perform the search.
(cog-recognize adv-sent)

;-------------------------------------------------------
;; Evaluate each of the bind links that were found.
(define ruleset (cog-recognize sent))

(map cog-bind (cog-outgoing-set ruleset))

; For the non-adverbial sentence this returns the below:
((SetLink
   (ListLink
      (ConceptNode "I")
      (ConceptNode "love")
      (ConceptNode "you")
      (ConceptNode "too")))
 (SetLink
   (ListLink
      (ConceptNode "I")
      (ConceptNode "like")
      (ConceptNode "you")
      (ConceptNode "a")
      (ConceptNode "lot!"))))

;-------------------------------------------------------

; A pattern with two globs in it.
; The types of the globs are constrained, because, if not constrained
; the globs can sometimes pick up on parts of the various patterns
; created above.  We really want them to only pick up on the "sentences"
; (strings of ConceptNodes).
(define a-love-b
	(BindLink
		(VariableList
			(TypedVariable (Glob "$A") (Type "ConceptNode"))
			(TypedVariable (Glob "$B") (Type "ConceptNode")))
		(ListLink
			(GlobNode "$A")
			(ConceptNode "love")
			(GlobNode "$B"))
		(ListLink
			(ConceptNode "I'm")
			(ConceptNode "sure")
			(ConceptNode "that")
			(GlobNode "$A")
			(ConceptNode "love")
			(GlobNode "$B"))))

; Lets see if the above can be found!
(cog-recognize adv-sent)

(define constrained-adv-sent
	(PatternLink
		(BindLink
			(VariableNode "$type constraints")
			(ListLink
				(ConceptNode "I")
				(ConceptNode "really")
				(ConceptNode "truly")
				(ConceptNode "love")
				(ConceptNode "you"))
			(VariableNode "$impl"))))

(cog-recognize constrained-adv-sent)

;-------------------------------------------------------
*unspecified*
