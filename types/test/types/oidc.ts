declare const binding: IdentityBinding;
const token: Promise<string> = binding.getToken();
const grant: Promise<string> = binding.getAuthorizationGrant({
  authorizationServer: 'https://auth.example.com',
  scope: ['read'],
});
const response: Promise<Response> = binding.fetch('https://api.example.com');
void [token, grant, response];
// @ts-expect-error Audience selection belongs in configuration.
binding.getToken({ audience: 'other' });
// @ts-expect-error Freshness is unconditional.
binding.getToken({ fresh: true });
// @ts-expect-error No signing-only API.
binding.signRequest('https://api.example.com');

const customer: Promise<IdentityCustomerIdentity> = binding.getCustomerIdentity('customer_x');
const customerResponse: Promise<Response> = binding.fetchForCustomer('customer_x', 'https://api.example.com');
void [customer, customerResponse];
// @ts-expect-error Only an opaque customer string, no authority override.
binding.getCustomerIdentity({ customerId: 'x', accountId: 'forged' });
