// SPDX-License-Identifier: Apache-2.0
// Copyright Authors of Cilium

// Code generated by client-gen. DO NOT EDIT.

package v1beta1

import (
	http "net/http"

	discoveryv1beta1 "github.com/cilium/cilium/pkg/k8s/slim/k8s/api/discovery/v1beta1"
	scheme "github.com/cilium/cilium/pkg/k8s/slim/k8s/client/clientset/versioned/scheme"
	rest "k8s.io/client-go/rest"
)

type DiscoveryV1beta1Interface interface {
	RESTClient() rest.Interface
	EndpointSlicesGetter
}

// DiscoveryV1beta1Client is used to interact with features provided by the discovery.k8s.io group.
type DiscoveryV1beta1Client struct {
	restClient rest.Interface
}

func (c *DiscoveryV1beta1Client) EndpointSlices(namespace string) EndpointSliceInterface {
	return newEndpointSlices(c, namespace)
}

// NewForConfig creates a new DiscoveryV1beta1Client for the given config.
// NewForConfig is equivalent to NewForConfigAndClient(c, httpClient),
// where httpClient was generated with rest.HTTPClientFor(c).
func NewForConfig(c *rest.Config) (*DiscoveryV1beta1Client, error) {
	config := *c
	setConfigDefaults(&config)
	httpClient, err := rest.HTTPClientFor(&config)
	if err != nil {
		return nil, err
	}
	return NewForConfigAndClient(&config, httpClient)
}

// NewForConfigAndClient creates a new DiscoveryV1beta1Client for the given config and http client.
// Note the http client provided takes precedence over the configured transport values.
func NewForConfigAndClient(c *rest.Config, h *http.Client) (*DiscoveryV1beta1Client, error) {
	config := *c
	setConfigDefaults(&config)
	client, err := rest.RESTClientForConfigAndClient(&config, h)
	if err != nil {
		return nil, err
	}
	return &DiscoveryV1beta1Client{client}, nil
}

// NewForConfigOrDie creates a new DiscoveryV1beta1Client for the given config and
// panics if there is an error in the config.
func NewForConfigOrDie(c *rest.Config) *DiscoveryV1beta1Client {
	client, err := NewForConfig(c)
	if err != nil {
		panic(err)
	}
	return client
}

// New creates a new DiscoveryV1beta1Client for the given RESTClient.
func New(c rest.Interface) *DiscoveryV1beta1Client {
	return &DiscoveryV1beta1Client{c}
}

func setConfigDefaults(config *rest.Config) {
	gv := discoveryv1beta1.SchemeGroupVersion
	config.GroupVersion = &gv
	config.APIPath = "/apis"
	config.NegotiatedSerializer = rest.CodecFactoryForGeneratedClient(scheme.Scheme, scheme.Codecs).WithoutConversion()

	if config.UserAgent == "" {
		config.UserAgent = rest.DefaultKubernetesUserAgent()
	}
}

// RESTClient returns a RESTClient that is used to communicate
// with API server by this client implementation.
func (c *DiscoveryV1beta1Client) RESTClient() rest.Interface {
	if c == nil {
		return nil
	}
	return c.restClient
}
