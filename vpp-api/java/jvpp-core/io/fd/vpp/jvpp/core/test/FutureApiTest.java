/*
 * Copyright (c) 2016 Cisco and/or its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package io.fd.vpp.jvpp.core.test;

import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.Future;
import java.util.logging.Level;
import java.util.logging.Logger;
import io.fd.vpp.jvpp.JVpp;
import io.fd.vpp.jvpp.JVppRegistry;
import io.fd.vpp.jvpp.JVppRegistryImpl;
import io.fd.vpp.jvpp.core.JVppCoreImpl;
import io.fd.vpp.jvpp.core.dto.BridgeDomainDetailsReplyDump;
import io.fd.vpp.jvpp.core.dto.BridgeDomainDump;
import io.fd.vpp.jvpp.core.dto.GetNodeIndex;
import io.fd.vpp.jvpp.core.dto.GetNodeIndexReply;
import io.fd.vpp.jvpp.core.dto.ShowVersion;
import io.fd.vpp.jvpp.core.dto.ShowVersionReply;
import io.fd.vpp.jvpp.core.dto.SwInterfaceDetails;
import io.fd.vpp.jvpp.core.dto.SwInterfaceDetailsReplyDump;
import io.fd.vpp.jvpp.core.dto.SwInterfaceDump;
import io.fd.vpp.jvpp.core.future.FutureJVppCoreFacade;

public class FutureApiTest {

    private static final Logger LOG = Logger.getLogger(FutureApiTest.class.getName());

    private static void testShowVersion(final FutureJVppCoreFacade jvpp) throws Exception {
        LOG.info("Sending ShowVersion request...");
        final Future<ShowVersionReply> replyFuture = jvpp.showVersion(new ShowVersion()).toCompletableFuture();
        final ShowVersionReply reply = replyFuture.get();
        LOG.info(
                String.format(
                        "Received ShowVersionReply: context=%d, program=%s, version=%s, buildDate=%s, buildDirectory=%s\n",
                        reply.context, new String(reply.program), new String(reply.version), new String(reply.buildDate),
                        new String(reply.buildDirectory)));
    }

    private static void testEmptyBridgeDomainDump(final FutureJVppCoreFacade jvpp) throws Exception {
        LOG.info("Sending ShowVersion request...");
        final BridgeDomainDump request = new BridgeDomainDump();
        request.bdId = -1; // dump call

        final CompletableFuture<BridgeDomainDetailsReplyDump>
                replyFuture = jvpp.bridgeDomainDump(request).toCompletableFuture();
        final BridgeDomainDetailsReplyDump reply = replyFuture.get();

        if (reply == null || reply.bridgeDomainDetails == null) {
            LOG.severe("Received null response for empty dump: " + reply);
        } else {
            LOG.info(
                    String.format(
                            "Received empty bridge-domain dump reply with list of bridge-domains: %s, %s",
                            reply.bridgeDomainDetails, reply.bridgeDomainSwIfDetails));
        }
    }

    private static void testGetNodeIndex(final FutureJVppCoreFacade jvpp) {
        LOG.info("Sending GetNodeIndex request...");
        final GetNodeIndex request = new GetNodeIndex();
        request.nodeName = "non-existing-node".getBytes();
        final Future<GetNodeIndexReply> replyFuture = jvpp.getNodeIndex(request).toCompletableFuture();
        try {
            final GetNodeIndexReply reply = replyFuture.get();
            LOG.info(
                    String.format(
                            "Received GetNodeIndexReply: context=%d, nodeIndex=%d\n", reply.context, reply.nodeIndex));
        } catch (Exception e) {
            LOG.log(Level.SEVERE, "GetNodeIndex request failed", e);
        }
    }

    private static void testSwInterfaceDump(final FutureJVppCoreFacade jvpp) throws Exception {
        LOG.info("Sending SwInterfaceDump request...");
        final SwInterfaceDump request = new SwInterfaceDump();
        request.nameFilterValid = 0;
        request.nameFilter = "".getBytes();

        final Future<SwInterfaceDetailsReplyDump> replyFuture = jvpp.swInterfaceDump(request).toCompletableFuture();
        final SwInterfaceDetailsReplyDump reply = replyFuture.get();
        for (SwInterfaceDetails details : reply.swInterfaceDetails) {
            Objects.requireNonNull(details, "reply.swInterfaceDetails contains null element!");
            LOG.info(
                    String.format("Received SwInterfaceDetails: interfaceName=%s, l2AddressLength=%d, adminUpDown=%d, "
                                    + "linkUpDown=%d, linkSpeed=%d, linkMtu=%d\n",
                            new String(details.interfaceName), details.l2AddressLength, details.adminUpDown,
                            details.linkUpDown, details.linkSpeed, (int) details.linkMtu));
        }
    }

    private static void testFutureApi() throws Exception {
        LOG.info("Testing Java future API");

        final JVppRegistry registry = new JVppRegistryImpl("FutureApiTest");
        final JVpp jvpp = new JVppCoreImpl();
        final FutureJVppCoreFacade jvppFacade = new FutureJVppCoreFacade(registry, jvpp);
        LOG.info("Successfully connected to VPP");

        testEmptyBridgeDomainDump(jvppFacade);
        testShowVersion(jvppFacade);
        testGetNodeIndex(jvppFacade);
        testSwInterfaceDump(jvppFacade);

        LOG.info("Disconnecting...");
        registry.close();
    }

    public static void main(String[] args) throws Exception {
        testFutureApi();
    }
}
