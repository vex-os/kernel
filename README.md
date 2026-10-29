# Vex

### 10/2026 experiment - Opus 5.5

> Claude helps a ton with mundane tasks like data analysis, which genuinely helps when you're developing a commercial software which math engine is supposed to exactly follow an another commercial software (I tell you what, how the HELL did PC-DMIS get a PTB certification is a myth to me) but I digress. The point being - I have never used Claude Opus 5.5 for personal projects and recent results from the software at work kind of made me wonder what Opus can do for something more, uh... lower-level?  

I kind of let Opus 5.5 analyze the existing Vex source code and make a document artifact with all the stuff found categorized into critical, serious and architectural issues, and then let it figure out a way to solve these issues, so quite a good amount of commits are Assisted-By Claude Opus 5.5. Rest assured though, I try to  :wheelchair: :wheelchair: :wheelchair: review, clean up and understand :wheelchair: :wheelchair: :wheelchair: the code it makes...

Discoveries so far:  

- Opus 5.5 is surprisingly smart for the amount of tokens it burns, sometimes too smart for its own good, so the "less tokens less problems" thing still applies to AI-assisted programming;  

- Opus 5.5 seems to forget a lot of things present in the project memory. This doesn't happen with `CLAUDE.md` which seems to be appended to the system prompt for each session;  

- LLMs in general make way better commit messages than I have ever done in my entire career lmao;  
