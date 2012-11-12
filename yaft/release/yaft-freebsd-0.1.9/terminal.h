/* See LICENSE for licence details. */
void init_cell(struct cell *cp)
{
	cp->code = DEFAULT_CHAR;
	cp->color.fg = DEFAULT_FG;
	cp->color.bg = DEFAULT_BG;
	cp->attribute = RESET;
	cp->wide = HALF;
}

void copy_cell(struct cell *dst, struct cell *src)
{
	*dst = *src;

	if (src->wide == WIDE) {
		*(dst + 1) = *src;
		(dst + 1)->wide = NEXT_TO_WIDE;
	}
}

void swap_color(struct color_pair *cp)
{
	int tmp;

	tmp = cp->fg;
	cp->fg = cp->bg;
	cp->bg = tmp;
}

int set_cell(struct terminal *term, int y, int x, uint16_t code)
{
	struct cell nc, *cp;
	struct glyph_t *gp;

	cp = &term->cells[x + y * term->cols];
	gp = term->fonts[code].gp;

	nc.code = code;

	nc.color.fg = (term->attribute & attr_mask[BOLD] && term->color.fg <= 7) ?
		term->color.fg + BRIGHT_INC: term->color.fg;
	nc.color.bg = (term->attribute & attr_mask[BLINK] && term->color.bg <= 7) ?
		term->color.bg + BRIGHT_INC: term->color.bg;

	if (term->attribute & attr_mask[REVERSE])
		swap_color(&nc.color);

	nc.attribute = term->attribute;
	nc.wide = (gp->width > term->cell_width) ? WIDE : HALF;

	*cp = nc;
	term->line_dirty[y] = true;

	if (nc.wide == WIDE && x + 1 < term->cols) {
		cp = &term->cells[x + 1 + y * term->cols];
		*cp = nc;
		cp->wide = NEXT_TO_WIDE;
		return WIDE;
	}

	return HALF;
}

void scroll(struct terminal *term, int from, int to, int offset)
{
	int i, j, size, abs_offset;
	struct cell *dst, *src;

	if (offset == 0 || from >= to)
		return;

	if (DEBUG)
		fprintf(stderr, "scroll from:%d to:%d offset:%d\n", from, to, offset);

	for (i = from; i <= to; i++)
		term->line_dirty[i] = true;

	abs_offset = abs(offset);
	size = sizeof(struct cell) * ((to - from + 1) - abs_offset) * term->cols;

	dst = term->cells + from * term->cols;
	src = term->cells + (from + abs_offset) * term->cols;

	if (offset > 0) {
		memmove(dst, src, size);
		for (i = (to - offset + 1); i <= to; i++)
			for (j = 0; j < term->cols; j++)
				init_cell(&term->cells[j + i * term->cols]);
	}
	else {
		memmove(src, dst, size);
		for (i = from; i < from + abs_offset; i++)
			for (j = 0; j < term->cols; j++)
				init_cell(&term->cells[j + i * term->cols]);
	}
}

/* relative movement: cause scrolling */
void move_cursor(struct terminal *term, int y_offset, int x_offset)
{
	int x, y, top, bottom;

	x = term->cursor.x + x_offset;
	y = term->cursor.y + y_offset;

	top = term->scroll.top;
	bottom = term->scroll.bottom;

	if (x < 0)
		x = 0;
	else if (x >= term->cols) {
		if (term->mode & MODE_AMRIGHT)
			term->wrap = true;
		x = term->cols - 1;
	}
	term->cursor.x = x;

	y = (y < 0) ? 0:
		(y >= term->lines) ? term->lines - 1: y;

	if (term->cursor.y == top && y_offset < 0) {
		y = top;
		scroll(term, top, bottom, y_offset);
	}
	else if (term->cursor.y == bottom && y_offset > 0) {
		y = bottom;
		scroll(term, top, bottom, y_offset);
	}
	term->cursor.y = y;
}

/* absolute movement: never scroll */
void set_cursor(struct terminal *term, int y, int x)
{
	int top, bottom;

	if (term->mode & MODE_ORIGIN) {
		top = term->scroll.top;
		bottom = term->scroll.bottom;
		y += term->scroll.top;
	}
	else {
		top = 0;
		bottom = term->lines - 1;
	}

	x = (x < 0) ? 0: (x >= term->cols) ? term->cols - 1: x;
	y = (y < top) ? top: (y > bottom) ? bottom: y;

	term->cursor.x = x;
	term->cursor.y = y;
}

void addch(struct terminal *term, uint32_t code)
{
	struct glyph_t *gp;

	if (code >= UCS2_CHARS				/* not print over UCS2 (>= 0x10000) */
		|| term->fonts[code].gp == NULL)/* glyph not found */
		return;

	gp = term->fonts[code].gp;			/* folding */
	if ((term->wrap && term->cursor.x == term->cols - 1)
		|| (gp->width > term->cell_width && term->cursor.x == term->cols - 1)) {
		set_cursor(term, term->cursor.y, 0);
		move_cursor(term, 1, 0);
	}
	term->wrap = false;

	move_cursor(term, 0,
		set_cell(term, term->cursor.y, term->cursor.x, code));
}

void reset_esc(struct terminal *term)
{
	if (DEBUG)
		fprintf(stderr, "*esc reset*\n");
	memset(term->esc.buf, '\0', BUFSIZE);
	term->esc.bp = term->esc.buf;
	term->esc.state = RESET;
}

bool push_esc(struct terminal *term, uint8_t ch)
{
	if (term->esc.bp == &term->esc.buf[BUFSIZE - 1]) {	/* buffer limit */
		reset_esc(term);
		return false;
	}

	*term->esc.bp++ = ch;
	if (term->esc.state == STATE_ESC) {
		if ('0' <= ch && ch <= '~')
			return true;
		else if (' ' > ch || ch > '/')
			reset_esc(term);
	}
	else if (term->esc.state == STATE_CSI) {
		if ('@' <= ch && ch <= '~')
			return true;
		else if (' ' > ch || ch > '?')
			reset_esc(term);
	}
	else if (term->esc.state == STATE_OSC) {
		if ((ch == BEL)
			|| (ch == BACKSLASH
			&& (term->esc.bp - term->esc.buf) >= 2
			&& *(term->esc.bp - 2) == ESC))
			return true;
		else if ((ch != ESC) && (' ' > ch || ch > '~'))
			reset_esc(term);
	}
	return false;
}

void reset_ucs(struct terminal *term)
{
	term->ucs.code = term->ucs.count = term->ucs.length = 0;
}

void reset_state(struct terminal *term)
{
	term->state.cursor.x = term->state.cursor.y = 0;
	term->state.attribute = RESET;
	term->state.mode = RESET;
}

void redraw(struct terminal *term)
{
	int i;

	for (i = 0; i < term->lines; i++)
		term->line_dirty[i] = true;
}

void reset(struct terminal *term)
{
	int i, j;

	term->mode = RESET;
	term->mode |= (MODE_CURSOR | MODE_AMRIGHT);
	term->wrap = false;

	term->scroll.top = 0;
	term->scroll.bottom = term->lines - 1;

	term->cursor.x = term->cursor.y = 0;

	term->color.fg = DEFAULT_FG;
	term->color.bg = DEFAULT_BG;

	term->attribute = RESET;

	for (i = 0; i < term->lines; i++) {
		for (j = 0; j < term->cols; j++) {
			init_cell(&term->cells[j + i * term->cols]);
			if ((j % TABSTOP) == 0)
				term->tabstop[j] = true;
			else
				term->tabstop[j] = false;
		}
		term->line_dirty[i] = true;
	}

	reset_state(term);
	reset_esc(term);
	reset_ucs(term);
}

void term_init(struct terminal *term, struct pair res)
{
	struct glyph_t *gp;

	load_fonts(term->fonts, font_path, glyph_alias);

	gp = term->fonts[DEFAULT_CHAR].gp;
	term->cell_width = gp->width;
	term->cell_height = gp->height;

	term->width = res.x;
	term->height = res.y;
	term->offset.x = term->offset.y = 0;

	term->cols = term->width / term->cell_width;
	term->lines = term->height / term->cell_height;

	if (DEBUG)
		fprintf(stderr, "width:%d height:%d cols:%d lines:%d\n",
			term->width, term->height, term->cols, term->lines);

	term->line_dirty = (bool *) emalloc(sizeof(bool) * term->lines);
	term->tabstop = (bool *) emalloc(sizeof(bool) * term->cols);

	term->cells = (struct cell *)
		emalloc(sizeof(struct cell) * term->cols * term->lines);
	reset(term);

	ewrite(STDIN_FILENO, (uint8_t *) "\033[?25l", 6);	/* cusor hidden */
}

void term_die(struct terminal *term)
{
	int i;

	for (i = 0; i < UCS2_CHARS; i++) {
		if (!term->fonts[i].is_alias) {
			free(term->fonts[i].gp->bitmap);
			free(term->fonts[i].gp);
		}
	}
	free(term->line_dirty);
	free(term->cells);

	ewrite(STDIN_FILENO, (uint8_t *) "\033[?25h", 6);	/* cursor visible */
}
