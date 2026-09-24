import { describe, expect, it } from 'vitest';
// As text rather than parsed: happy-dom's parser fetches the page's stylesheets.
import markup from '../index.html?raw';

describe('the page as it is served', () => {
  it('ships the start button disabled, so a press before the core is ready is not lost', () => {
    // `main.ts` enables it once its listener is attached. Until then a press would do nothing,
    // and on a reload the button beside it that would have resumed is not there yet (ADR 0063).
    const tags = markup.match(/<button\b[^>]*\bid="enable"[^>]*>/g) ?? [];
    expect(tags).toHaveLength(1);
    expect(tags[0]).toMatch(/\sdisabled[\s>=]/);
  });
});
