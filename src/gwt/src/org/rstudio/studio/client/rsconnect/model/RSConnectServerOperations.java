/*
 * RSConnectServerOperations.java
 *
 * Copyright (C) 2009-14 by RStudio, Inc.
 *
 * Unless you have received this program directly from RStudio pursuant
 * to the terms of a commercial license agreement with RStudio, then
 * this program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */
package org.rstudio.studio.client.rsconnect.model;

import org.rstudio.studio.client.server.ServerRequestCallback;
import org.rstudio.studio.client.server.Void;

import com.google.gwt.core.client.JsArray;
import com.google.gwt.core.client.JsArrayString;

public interface RSConnectServerOperations
{
   void removeRSConnectAccount(String accountName, 
               ServerRequestCallback<Void> requestCallback);

   void getRSConnectAccountList(
               ServerRequestCallback<JsArrayString> requestCallback);

   void connectRSConnectAccount(String command, 
               ServerRequestCallback<Void> requestCallback);

   void getRSConnectAppList(String accountName,
               ServerRequestCallback<JsArray<RSConnectApplicationInfo>> requestCallback);
   
   void getRSConnectDeployments(String dir, 
               ServerRequestCallback<JsArray<RSConnectDeploymentRecord>> requestCallback); 
   
   void getDeploymentFiles (String dir, 
               ServerRequestCallback<RSConnectDeploymentFiles> requestCallback);
   
   void deployShinyApp(String dir, String file, String account, String appName, 
               ServerRequestCallback<Boolean> requestCallback);
}
