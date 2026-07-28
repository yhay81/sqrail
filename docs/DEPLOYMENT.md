# Documentation deployment

> `sqrails.yhay81.com` is a static-assets Worker with configuration kept in the
> repository.

[Documentation index](README.md) ·
[Cloudflare Worker configuration](../wrangler.jsonc) ·
[Published site](https://sqrails.yhay81.com)

## Architecture

Cloudflare Workers Static Assets serves the contents of `site/`. The
`wrangler.jsonc` file is the source of truth for:

- Worker name: `sqrails`
- custom domain: `sqrails.yhay81.com`
- HTML path and custom 404 behavior
- static asset directory

The custom domain is configured as a Worker origin. Cloudflare manages its DNS
record and TLS certificate.

GitHub Pages is intentionally disabled. Keeping a second public deployment
would create a stale-documentation risk and an ambiguous canonical URL.
Canonical, Open Graph, sitemap, repository, and agent-discovery URLs all use
`https://sqrails.yhay81.com`.

## Validate locally

Node.js 24 or newer is required only for site development:

```sh
npm ci
npm run site:check
```

This checks formatting, HTML structure and accessibility rules, JavaScript
syntax, and a Wrangler deployment dry run. The C++ CI build separately verifies
that `site/agent-help.txt` exactly matches `sqrail --agent-help`.

## Deploy

Authenticate Wrangler with the Cloudflare account that owns `yhay81.com`, then:

```sh
npm ci
npm run deploy:cloudflare
```

For Cloudflare Workers Builds, connect `yhay81/sqrail`, select `main` as the
production branch, use `npm ci` as the build command, and use
`npm run deploy:cloudflare` as the deploy command. Preview branches can use
`npx wrangler versions upload`.

Do not add a Cloudflare API token to the repository. Local Wrangler OAuth or the
Cloudflare-managed Workers Builds integration supplies deployment
authentication.

## Verify production

After deployment, check the origin, headers, documents, and custom 404:

```sh
curl --fail --silent --show-error --head https://sqrails.yhay81.com/
curl --fail --silent --show-error https://sqrails.yhay81.com/agent-help.txt
curl --fail --silent --show-error https://sqrails.yhay81.com/llms.txt
curl --silent --show-error --output /dev/null --write-out '%{http_code}\n' \
  https://sqrails.yhay81.com/not-a-real-path
```

The last command must report `404`. The main response must include the security
headers declared in `site/_headers`.
