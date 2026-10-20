# PyPI trusted publisher

In https://pypi.org/manage/account/publishing/ choose **Add a new pending publisher**,
then **GitHub**, and enter:

| Field | Value |
| --- | --- |
| PyPI project name | avenc |
| GitHub owner | filippi |
| Repository | avenc |
| Workflow filename | release.yml |
| Environment | pypi |

The author configured this on 20 October 2026. The first successful OIDC upload
creates the project and converts the pending publisher to a project publisher.
No API token is required. TestPyPI needs its own publisher with environment
`testpypi` if used. See RELEASE.md for the existing build and publication gates.
