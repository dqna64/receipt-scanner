type Child = Node | string | number | null | undefined | false | Child[];
type Props = Record<string, unknown> | null;

// h('button', {class: 'primary', onclick}, 'Save') -> element. Props starting with "on" become
// listeners; null/false children are skipped. The one render helper the views share.
export function h<K extends keyof HTMLElementTagNameMap>(tag: K, props: Props = {}, ...children: Child[]):
    HTMLElementTagNameMap[K] {
  const el = document.createElement(tag);
  for (const [key, value] of Object.entries(props ?? {})) {
    if (value === null || value === undefined || value === false) continue;
    if (key.startsWith('on')) {
      el.addEventListener(key.slice(2), value as EventListener);
    } else if (key === 'class') {
      el.className = String(value);
    } else if (value === true) {
      el.setAttribute(key, '');
    } else {
      el.setAttribute(key, String(value));
    }
  }
  appendChildren(el, children);
  return el;
}

function appendChildren(el: HTMLElement, children: Child[]): void {
  for (const child of children) {
    if (child === null || child === undefined || child === false) continue;
    if (Array.isArray(child)) appendChildren(el, child);
    else el.append(child instanceof Node ? child : String(child));
  }
}

// Amounts are integer cents, 1/100 of the main unit for every currency (spec Money).
export function formatMoney(cents: number, currency: string): string {
  try {
    return new Intl.NumberFormat(undefined, { style: 'currency', currency }).format(cents / 100);
  } catch {
    return `${(cents / 100).toFixed(2)} ${currency}`;
  }
}
