TEX     := pdflatex
TEXOPTS := -interaction=nonstopmode -halt-on-error -file-line-error
SRC     := main.tex
PDF     := main.pdf
FIGS    := convergence.pdf

ZIP     := overleaf.zip

.PHONY: all clean distclean figures overleaf

all: $(PDF)

$(PDF): $(SRC) $(FIGS)
	$(TEX) $(TEXOPTS) $(SRC)
	$(TEX) $(TEXOPTS) $(SRC)

figures: $(FIGS)

convergence.pdf: plot_convergence.py pyproject.toml
	uv run --project . python plot_convergence.py

clean:
	rm -f *.aux *.log *.nav *.out *.snm *.toc *.vrb *.synctex.gz

distclean: clean
	rm -f $(PDF) $(FIGS) $(ZIP)

# Pack a zip of the poster sources + pre-built figure for upload to Overleaf.
# Overleaf compiles main.tex with pdflatex; convergence.pdf is included pre-built
# so the project does not need Python/uv on Overleaf.
overleaf: $(FIGS)
	rm -f $(ZIP)
	zip $(ZIP) main.tex Makefile *.sty $(FIGS)
	@echo "Wrote $(ZIP) -- upload this to Overleaf."
