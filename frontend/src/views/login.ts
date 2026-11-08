import { api, ApiError } from '../api';
import { h } from '../dom';

// Log in, or register then log in. Calls onDone() once a session cookie is set.
export function loginView(container: HTMLElement, { onDone }: { onDone: () => void }): void {
  let registering = false;
  const email = h('input', { type: 'email', name: 'email', autocomplete: 'email', required: true });
  const password = h('input', { type: 'password', name: 'password', autocomplete: 'current-password',
                                required: true, minlength: 8 });
  const error = h('p', { class: 'error', role: 'alert' });
  const submit = h('button', { class: 'primary', type: 'submit' }, 'Log in');
  const toggle = h('button', { type: 'button', class: 'link', onclick: () => setMode(!registering) },
                   'Create an account');

  function setMode(register: boolean): void {
    registering = register;
    submit.textContent = register ? 'Create account' : 'Log in';
    toggle.textContent = register ? 'I already have an account' : 'Create an account';
    password.autocomplete = register ? 'new-password' : 'current-password';
    error.textContent = '';
  }

  async function onSubmit(event: SubmitEvent): Promise<void> {
    event.preventDefault();
    error.textContent = '';
    submit.disabled = true;
    const credentials = { email: email.value.trim(), password: password.value };
    try {
      if (registering) {
        await api('/auth/register', { method: 'POST', json: credentials });
      }
      await api('/auth/login', { method: 'POST', json: credentials });
      onDone();
    } catch (e) {
      error.textContent = e instanceof ApiError && e.status === 401 ? 'Wrong email or password.'
        : e instanceof Error ? e.message : String(e);
    } finally {
      submit.disabled = false;
    }
  }

  container.replaceChildren(
    h('form', { class: 'card stack', onsubmit: onSubmit },
      h('h1', {}, 'Receipts'),
      h('label', {}, 'Email', email),
      h('label', {}, 'Password', password),
      error, submit, toggle));
  email.focus();
}
